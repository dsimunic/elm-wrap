/**
 * review.c - Review command group for running rulr rules against Elm files
 *
 * This command integrates the rulr (Mini Datalog) engine to run user-defined
 * rules against parsed Elm source files and elm.json project configuration.
 *
 * Host-generated facts are extracted from:
 *   - Elm source file AST (module, imports, declarations, types)
 *   - elm.json project configuration (dependencies, type, version)
 *
 * Each rule file is run in sequence and results are printed after each run.
 */

#include "review.h"
#include "reporter.h"
#include "../../alloc.h"
#include "../../progname.h"
#include "../../elm_json.h"
#include "../../fileutil.h"
#include "../../ast/skeleton.h"
#include "../../ast/util.h"
#include "../../dyn_array.h"
#include "../../cache.h"
#include "../../vendor/cJSON.h"
#include "../../rulr/rulr.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/host_helpers.h"
#include "../../rulr/engine/engine.h"
#include "../../rulr/runtime/runtime.h"
#include "../../rulr/common/types.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

/* ============================================================================
 * Usage
 * ========================================================================== */

static void print_review_usage(void) {
    printf("Usage: %s review SUBCOMMAND [OPTIONS]\n", program_name);
    printf("\n");
    printf("Run rulr rules against Elm source files for code review.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  file <FILE>        Analyze an Elm source file with rulr rules\n");
    printf("  package <PATH>     Analyze an Elm package directory with rulr rules\n");
    printf("\n");
    printf("Options:\n");
    printf("  -q, --quiet        Quiet mode: no output, exit 100 on first error, 0 if OK\n");
    printf("  -h, --help         Show this help message\n");
}

static void print_file_usage(void) {
    printf("Usage: %s review file <FILE> [OPTIONS]\n", program_name);
    printf("\n");
    printf("Analyze an Elm source file using rulr (Datalog) rules.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <FILE>             Path to Elm source file (.elm)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --config <PATH>    Path to elm.json (default: auto-detect in parent dirs)\n");
    printf("  --rule <NAME>      Rule name or path (without extension) - can be repeated\n");
    printf("                     Tries .dlc (compiled) first, falls back to .dl (source)\n");
    printf("  -q, --quiet        Quiet mode: no output, exit 100 on first error, 0 if OK\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s review file src/Main.elm --rule rules/no-debug\n", program_name);
    printf("  %s review file src/Main.elm --config elm.json --rule a --rule b\n", program_name);
    printf("\n");
    printf("Host-generated facts available in rules:\n");
    printf("  module(name)                   - Module name\n");
    printf("  import(module)                 - Imported modules\n");
    printf("  import_alias(module, alias)    - Import aliases\n");
    printf("  import_exposing(module, name)  - Exposed imports\n");
    printf("  type_annotation(name, type)    - Type annotations\n");
    printf("  sig_uses_type(func, type)      - Function signature uses a type\n");
    printf("  type_alias(name)               - Type alias declarations\n");
    printf("  union_type(name)               - Union type declarations\n");
    printf("  constructor(type, name)        - Union type constructors\n");
    printf("  exported_value(name)           - Exported values/functions\n");
    printf("  exported_type(name)            - Exported types\n");
    printf("  file_path(path)                - Source file path\n");
    printf("  project_type(type)             - Project type (application/package)\n");
    printf("  elm_version(version)           - Elm version from elm.json\n");
    printf("  dependency(author, package, version) - Direct dependencies\n");
    printf("  package_module(author, package, module) - Modules exposed by a dependency\n");
}

static void print_package_usage(void) {
    printf("Usage: %s review package <PATH> [OPTIONS]\n", program_name);
    printf("\n");
    printf("Analyze an Elm package directory using rulr (Datalog) rules.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <PATH>             Path to package directory (must contain elm.json, src/)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --rule <NAME>      Rule name or path (without extension) - can be repeated\n");
    printf("                     Tries .dlc (compiled) first, falls back to .dl (source)\n");
    printf("  -q, --quiet        Quiet mode: no output, exit 100 on first error, 0 if OK\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s review package /path/to/package --rule rules/no_redundant_files\n", program_name);
    printf("\n");
    printf("Host-generated facts available in rules:\n");
    printf("  exposed_module(module)         - Modules from exposed-modules in elm.json\n");
    printf("  file_module(file, module)      - Mapping from file path to module name\n");
    printf("  file_import(file, module)      - Import statements in a file\n");
    printf("  source_file(file)              - All .elm files in src/ directory\n");
    printf("  package_file(file)             - All files (absolute path)\n");
    printf("  package_file_rel(path)         - All files (relative to package root)\n");
    printf("  package_file_name(name)        - All filenames (just the name)\n");
    printf("  package_file_info(abs, rel, name) - Combined file info\n");
    printf("  allowed_root_file(file)        - LICENSE, README.md, elm.json\n");
}

/* ============================================================================
 * Fact generation helpers
 * ========================================================================== */

/**
 * Read file contents into arena-allocated string
 */
static char *read_file_content(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = arena_malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(content, 1, fsize, f);
    content[read_size] = '\0';
    fclose(f);

    return content;
}

/**
 * Extract type references from a type string and insert sig_uses_type facts.
 * This scans the type string for uppercase identifiers (type names).
 * 
 * Type references are identified as:
 * - Uppercase identifiers that start a word (e.g., "Happiness", "String", "List")
 * - Handles qualified names (e.g., "Html.Html" -> "Html")
 */
static void extract_type_references(Rulr *r, const char *func_name, const char *type_str) {
    if (!type_str) return;
    
    const char *p = type_str;
    while (*p) {
        /* Skip until we find an uppercase letter that starts a type name */
        while (*p && !isupper((unsigned char)*p)) {
            p++;
        }
        if (!*p) break;
        
        /* Extract the type name */
        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) {
            p++;
        }
        
        size_t len = p - start;
        if (len > 0) {
            char *type_name = arena_malloc(len + 1);
            if (type_name) {
                memcpy(type_name, start, len);
                type_name[len] = '\0';
                
                /* If it's a qualified name like "Html.Html", just use the first part */
                char *dot = strchr(type_name, '.');
                if (dot) {
                    *dot = '\0';
                }
                
                /* Insert sig_uses_type(func_name, type_name) fact */
                rulr_insert_fact_2s(r, "sig_uses_type", func_name, type_name);
                
                arena_free(type_name);
            }
        }
    }
}

/* ============================================================================
 * Fact extraction from skeleton AST
 * ========================================================================== */

static void extract_module_facts(Rulr *r, const SkeletonModule *mod) {
    /* File path fact */
    if (mod->filepath) {
        rulr_insert_fact_1s(r, "file_path", mod->filepath);
    }

    /* Module name fact */
    if (mod->module_name) {
        rulr_insert_fact_1s(r, "module", mod->module_name);
    }

    /* Exported values */
    if (mod->exports.expose_all) {
        /* All values are exported - mark with special symbol */
        rulr_insert_fact_1s(r, "export_all", "true");
    }
    for (int i = 0; i < mod->exports.values_count; i++) {
        rulr_insert_fact_1s(r, "exported_value", mod->exports.values[i]);
    }
    for (int i = 0; i < mod->exports.types_count; i++) {
        rulr_insert_fact_1s(r, "exported_type", mod->exports.types[i]);
    }
    for (int i = 0; i < mod->exports.types_with_constructors_count; i++) {
        rulr_insert_fact_1s(r, "exported_type_with_constructors", mod->exports.types_with_constructors[i]);
    }

    /* Imports */
    for (int i = 0; i < mod->imports_count; i++) {
        const SkeletonImport *imp = &mod->imports[i];
        if (imp->module_name) {
            rulr_insert_fact_1s(r, "import", imp->module_name);

            if (imp->alias) {
                rulr_insert_fact_2s(r, "import_alias", imp->module_name, imp->alias);
            }

            if (imp->expose_all) {
                rulr_insert_fact_2s(r, "import_expose_all", imp->module_name, "true");
            }

            for (int j = 0; j < imp->exposed_values_count; j++) {
                rulr_insert_fact_2s(r, "import_exposing", imp->module_name, imp->exposed_values[j]);
            }
            for (int j = 0; j < imp->exposed_types_count; j++) {
                rulr_insert_fact_2s(r, "import_exposing_type", imp->module_name, imp->exposed_types[j]);
            }
        }
    }

    /* Type annotations */
    for (int i = 0; i < mod->type_annotations_count; i++) {
        const SkeletonTypeAnnotation *ann = &mod->type_annotations[i];
        if (ann->name) {
            /* Try to get the type string: first canonical, then qualified, then raw from node */
            const char *type_str = NULL;
            char *raw_type_str = NULL;
            
            if (ann->canonical_type) {
                type_str = ann->canonical_type;
            } else if (ann->qualified_type) {
                type_str = ann->qualified_type;
            } else if (!ts_node_is_null(ann->type_node) && mod->source_code) {
                /* Extract raw type string from the AST node */
                raw_type_str = ast_get_node_text(ann->type_node, mod->source_code);
                type_str = raw_type_str ? raw_type_str : "(unknown)";
            } else {
                type_str = "(unknown)";
            }
            
            rulr_insert_fact_2s(r, "type_annotation", ann->name, type_str);
            
            /* Extract type references for sig_uses_type facts */
            if (type_str && strcmp(type_str, "(unknown)") != 0) {
                extract_type_references(r, ann->name, type_str);
            }
        }
    }

    /* Type aliases */
    for (int i = 0; i < mod->type_aliases_count; i++) {
        const SkeletonTypeAlias *alias = &mod->type_aliases[i];
        if (alias->name) {
            rulr_insert_fact_1s(r, "type_alias", alias->name);
        }
    }

    /* Union types and constructors */
    for (int i = 0; i < mod->union_types_count; i++) {
        const SkeletonUnionType *ut = &mod->union_types[i];
        if (ut->name) {
            rulr_insert_fact_1s(r, "union_type", ut->name);

            for (int j = 0; j < ut->constructors_count; j++) {
                const SkeletonUnionConstructor *ctor = &ut->constructors[j];
                if (ctor->name) {
                    rulr_insert_fact_2s(r, "constructor", ut->name, ctor->name);
                }
            }
        }
    }

    /* Infix operators */
    for (int i = 0; i < mod->infixes_count; i++) {
        const SkeletonInfix *infix = &mod->infixes[i];
        if (infix->operator && infix->function_name) {
            rulr_insert_fact_2s(r, "infix", infix->operator, infix->function_name);
        }
    }
}

/* ============================================================================
 * Fact extraction from elm.json
 * ========================================================================== */

static void extract_elm_json_facts(Rulr *r, const ElmJson *ej) {
    /* Project type */
    const char *project_type = (ej->type == ELM_PROJECT_APPLICATION) ? "application" : "package";
    rulr_insert_fact_1s(r, "project_type", project_type);

    /* Elm version */
    if (ej->elm_version) {
        rulr_insert_fact_1s(r, "elm_version", ej->elm_version);
    }

    /* Package name (for packages) */
    if (ej->package_name) {
        rulr_insert_fact_1s(r, "package_name", ej->package_name);
    }
    if (ej->package_version) {
        rulr_insert_fact_1s(r, "package_version", ej->package_version);
    }

    /* Direct dependencies */
    if (ej->dependencies_direct) {
        for (int i = 0; i < ej->dependencies_direct->count; i++) {
            Package *pkg = &ej->dependencies_direct->packages[i];
            rulr_insert_fact_3s(r, "dependency", pkg->author, pkg->name, pkg->version);
        }
    }

    /* Package dependencies (for packages) */
    if (ej->package_dependencies) {
        for (int i = 0; i < ej->package_dependencies->count; i++) {
            Package *pkg = &ej->package_dependencies->packages[i];
            rulr_insert_fact_3s(r, "dependency", pkg->author, pkg->name, pkg->version);
        }
    }

    /* Indirect dependencies */
    if (ej->dependencies_indirect) {
        for (int i = 0; i < ej->dependencies_indirect->count; i++) {
            Package *pkg = &ej->dependencies_indirect->packages[i];
            rulr_insert_fact_3s(r, "indirect_dependency", pkg->author, pkg->name, pkg->version);
        }
    }

    /* Test dependencies */
    if (ej->dependencies_test_direct) {
        for (int i = 0; i < ej->dependencies_test_direct->count; i++) {
            Package *pkg = &ej->dependencies_test_direct->packages[i];
            rulr_insert_fact_3s(r, "test_dependency", pkg->author, pkg->name, pkg->version);
        }
    }
}

/* ============================================================================
 * Package module fact extraction (from ELM_HOME cached packages)
 * ========================================================================== */

/**
 * Parse exposed-modules from a package's elm.json
 * Returns array of module names (caller should not free - uses arena)
 */
static char **parse_package_exposed_modules(const char *elm_json_path, int *count) {
    *count = 0;
    
    char *content = read_file_content(elm_json_path);
    if (!content) return NULL;

    cJSON *root = cJSON_Parse(content);
    arena_free(content);
    if (!root) return NULL;

    int modules_capacity = 16;
    int modules_count = 0;
    char **modules = arena_malloc(modules_capacity * sizeof(char*));
    if (!modules) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *exposed = cJSON_GetObjectItem(root, "exposed-modules");
    if (!exposed) {
        cJSON_Delete(root);
        *count = 0;
        return modules;
    }

    /* Handle both array and object (categorized) formats */
    if (cJSON_IsArray(exposed)) {
        cJSON *item;
        cJSON_ArrayForEach(item, exposed) {
            if (cJSON_IsString(item)) {
                DYNARRAY_PUSH(modules, modules_count, modules_capacity, 
                             arena_strdup(item->valuestring), char*);
            }
        }
    } else if (cJSON_IsObject(exposed)) {
        /* Categorized format: { "Category": ["Module1", "Module2"], ... } */
        cJSON *category;
        cJSON_ArrayForEach(category, exposed) {
            if (cJSON_IsArray(category)) {
                cJSON *item;
                cJSON_ArrayForEach(item, category) {
                    if (cJSON_IsString(item)) {
                        DYNARRAY_PUSH(modules, modules_count, modules_capacity,
                                     arena_strdup(item->valuestring), char*);
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    *count = modules_count;
    return modules;
}

/**
 * Helper to process packages from a PackageMap and insert package_module facts.
 */
static void process_package_map_for_modules(Rulr *r, PackageMap *pkg_map, CacheConfig *cache) {
    if (!pkg_map) return;
    
    for (int i = 0; i < pkg_map->count; i++) {
        Package *pkg = &pkg_map->packages[i];
        
        /* Build path to package's elm.json in ELM_HOME cache */
        char *pkg_path = cache_get_package_path(cache, pkg->author, pkg->name, pkg->version);
        if (!pkg_path) continue;
        
        size_t elm_json_len = strlen(pkg_path) + 12; /* /elm.json\0 */
        char *pkg_elm_json = arena_malloc(elm_json_len);
        if (!pkg_elm_json) {
            arena_free(pkg_path);
            continue;
        }
        snprintf(pkg_elm_json, elm_json_len, "%s/elm.json", pkg_path);
        
        /* Parse exposed modules */
        int module_count = 0;
        char **modules = parse_package_exposed_modules(pkg_elm_json, &module_count);
        
        if (modules) {
            for (int m = 0; m < module_count; m++) {
                if (modules[m]) {
                    rulr_insert_fact_3s(r, "package_module", pkg->author, pkg->name, modules[m]);
                }
            }
        }
        
        arena_free(pkg_elm_json);
        arena_free(pkg_path);
    }
}

/**
 * Extract package_module facts for all direct dependencies.
 * For each dependency, reads the package's elm.json from ELM_HOME cache
 * to get its exposed modules, then inserts package_module(author, package, module) facts.
 */
static void extract_package_module_facts(Rulr *r, const ElmJson *ej, CacheConfig *cache) {
    if (!cache || !cache->packages_dir) return;

    /* Process direct dependencies */
    process_package_map_for_modules(r, ej->dependencies_direct, cache);
    
    /* Process package dependencies (for packages) */
    process_package_map_for_modules(r, ej->package_dependencies, cache);
}

/* ============================================================================
 * Result printing
 * ========================================================================== */
static void print_value(const Rulr *r, const Value *v) {
    switch (v->kind) {
    case VAL_SYM: {
        const char *name = rulr_lookup_symbol(r, v->u.sym);
        if (name) {
            printf("%s", name);
        } else {
            printf("#%d", v->u.sym);
        }
        break;
    }
    case VAL_RANGE:
        printf("range(%ld)", v->u.i);
        break;
    case VAL_INT:
        printf("%ld", v->u.i);
        break;
    default:
        printf("?");
        break;
    }
}

static void print_relation(const char *pred_name, const Rulr *r, EngineRelationView view) {
    const Tuple *tuples = (const Tuple *)view.tuples;
    for (int i = 0; i < view.num_tuples; ++i) {
        const Tuple *t = &tuples[i];
        printf("  %s(", pred_name);
        for (int a = 0; a < t->arity; ++a) {
            print_value(r, &t->fields[a]);
            if (a + 1 < t->arity) {
                printf(", ");
            }
        }
        printf(")\n");
    }
}

/* ============================================================================
 * Find elm.json in parent directories
 * ========================================================================== */

static char *find_elm_json(const char *start_path) {
    char *path = arena_strdup(start_path);
    if (!path) return NULL;

    /* Remove filename if it's a file */
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        *last_slash = '\0';
    }

    /* Search upward for elm.json */
    while (strlen(path) > 1) {
        size_t dir_len = strlen(path);
        char *elm_json_path = arena_malloc(dir_len + 12);
        if (!elm_json_path) {
            arena_free(path);
            return NULL;
        }
        snprintf(elm_json_path, dir_len + 12, "%s/elm.json", path);

        FILE *f = fopen(elm_json_path, "r");
        if (f) {
            fclose(f);
            arena_free(path);
            return elm_json_path;
        }
        arena_free(elm_json_path);

        /* Go up one directory */
        last_slash = strrchr(path, '/');
        if (last_slash) {
            *last_slash = '\0';
        } else {
            break;
        }
    }

    arena_free(path);
    return NULL;
}

/* ============================================================================
 * File subcommand implementation
 * ========================================================================== */

int cmd_review_file(int argc, char *argv[]) {
    const char *elm_file = NULL;
    const char *config_path = NULL;
    char **rule_files = NULL;
    int rule_files_count = 0;
    int rule_files_capacity = 4;
    int quiet_mode = 0;

    rule_files = arena_malloc(rule_files_capacity * sizeof(char*));
    if (!rule_files) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_file_usage();
            return 0;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet_mode = 1;
        } else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --config requires a path argument\n");
                return 1;
            }
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--rule") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --rule requires a path argument\n");
                return 1;
            }
            if (rule_files_count >= rule_files_capacity) {
                rule_files_capacity *= 2;
                rule_files = arena_realloc(rule_files, rule_files_capacity * sizeof(char*));
                if (!rule_files) {
                    fprintf(stderr, "Error: Out of memory\n");
                    return 1;
                }
            }
            rule_files[rule_files_count++] = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!elm_file) {
                elm_file = argv[i];
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    /* Validate arguments */
    if (!elm_file) {
        fprintf(stderr, "Error: No Elm file specified\n");
        print_file_usage();
        return 1;
    }

    if (rule_files_count == 0) {
        fprintf(stderr, "Error: At least one --rule file is required\n");
        print_file_usage();
        return 1;
    }

    /* Parse the Elm file */
    SkeletonModule *mod = skeleton_parse(elm_file);
    if (!mod) {
        fprintf(stderr, "Error: Failed to parse Elm file '%s'\n", elm_file);
        return 1;
    }

    /* Find or load elm.json */
    ElmJson *elm_json = NULL;
    if (config_path) {
        elm_json = elm_json_read(config_path);
        if (!elm_json) {
            if (!quiet_mode) {
                fprintf(stderr, "Warning: Failed to read elm.json at '%s'\n", config_path);
            }
        }
    } else {
        /* Try to auto-detect elm.json */
        char *detected_path = find_elm_json(elm_file);
        if (detected_path) {
            elm_json = elm_json_read(detected_path);
            if (elm_json && !quiet_mode) {
                printf("Using elm.json: %s\n", detected_path);
            }
            arena_free(detected_path);
        }
    }

    /* Initialize cache for package module fact extraction */
    CacheConfig *cache = NULL;
    if (elm_json) {
        cache = cache_config_init();
    }

    if (!quiet_mode) {
        printf("Reviewing: %s\n", elm_file);
        if (mod->module_name) {
            printf("Module: %s\n", mod->module_name);
        }
        printf("\n");
    }

    /* Run each rule file */
    int total_errors = 0;
    for (int r = 0; r < rule_files_count; r++) {
        const char *rule_path = rule_files[r];

        if (!quiet_mode) {
            printf("=== Rule file: %s ===\n", rule_path);
        }

        /* Initialize rulr engine */
        Rulr rulr;
        RulrError err = rulr_init(&rulr);
        if (err.is_error) {
            if (!quiet_mode) {
                fprintf(stderr, "Error: Failed to initialize rulr engine: %s\n", err.message);
            }
            continue;
        }

        /* Load the rule file (tries compiled .dlc first, then source .dl) */
        err = rulr_load_rule_file(&rulr, rule_path);
        if (err.is_error) {
            if (!quiet_mode) {
                fprintf(stderr, "Error: Failed to load rule file: %s\n", err.message);
            }
            rulr_deinit(&rulr);
            continue;
        }

        /* Insert host-generated facts from Elm file */
        extract_module_facts(&rulr, mod);

        /* Insert facts from elm.json if available */
        if (elm_json) {
            extract_elm_json_facts(&rulr, elm_json);
            /* Extract package_module facts from cached packages */
            if (cache) {
                extract_package_module_facts(&rulr, elm_json, cache);
            }
        }

        /* Evaluate the rules */
        err = rulr_evaluate(&rulr);
        if (err.is_error) {
            if (!quiet_mode) {
                fprintf(stderr, "Error: Rule evaluation failed: %s\n", err.message);
            }
            rulr_deinit(&rulr);
            continue;
        }

        /* Get and print the 'error' relation */
        EngineRelationView error_view = rulr_get_relation(&rulr, "error");
        if (error_view.pred_id >= 0 && error_view.num_tuples > 0) {
            total_errors += error_view.num_tuples;
            if (quiet_mode) {
                /* In quiet mode, bail on first error with exit code 100 */
                rulr_deinit(&rulr);
                skeleton_free(mod);
                if (elm_json) elm_json_free(elm_json);
                if (cache) cache_config_free(cache);
                return 100;
            }
            printf("Found %d error(s):\n", error_view.num_tuples);
            print_relation("error", &rulr, error_view);
        } else if (!quiet_mode) {
            printf("No errors found.\n");
        }

        /* Also check for 'warning' relation */
        EngineRelationView warning_view = rulr_get_relation(&rulr, "warning");
        if (!quiet_mode && warning_view.pred_id >= 0 && warning_view.num_tuples > 0) {
            printf("Found %d warning(s):\n", warning_view.num_tuples);
            print_relation("warning", &rulr, warning_view);
        }

        /* Check for 'info' relation */
        EngineRelationView info_view = rulr_get_relation(&rulr, "info");
        if (!quiet_mode && info_view.pred_id >= 0 && info_view.num_tuples > 0) {
            printf("Found %d info message(s):\n", info_view.num_tuples);
            print_relation("info", &rulr, info_view);
        }

        if (!quiet_mode) {
            printf("\n");
        }
        rulr_deinit(&rulr);
    }

    /* Cleanup */
    skeleton_free(mod);
    if (elm_json) {
        elm_json_free(elm_json);
    }
    if (cache) {
        cache_config_free(cache);
    }

    if (!quiet_mode) {
        printf("Total errors: %d\n", total_errors);
    }
    return total_errors > 0 ? 1 : 0;
}

/* ============================================================================
 * Package subcommand helpers
 * ========================================================================== */

/**
 * Parse elm.json exposed-modules for packages
 */
static char **pkg_parse_exposed_modules(const char *elm_json_path, int *count) {
    char *content = read_file_content(elm_json_path);
    if (!content) return NULL;

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        arena_free(content);
        return NULL;
    }

    int modules_capacity = 16;
    int modules_count = 0;
    char **modules = arena_malloc(modules_capacity * sizeof(char*));

    cJSON *exposed = cJSON_GetObjectItem(root, "exposed-modules");
    if (!exposed) {
        cJSON_Delete(root);
        arena_free(content);
        *count = 0;
        return modules;
    }

    /* Handle both array and object (categorized) formats */
    if (cJSON_IsArray(exposed)) {
        cJSON *item;
        cJSON_ArrayForEach(item, exposed) {
            if (cJSON_IsString(item)) {
                DYNARRAY_PUSH(modules, modules_count, modules_capacity, 
                             arena_strdup(item->valuestring), char*);
            }
        }
    } else if (cJSON_IsObject(exposed)) {
        /* Categorized format: { "Category": ["Module1", "Module2"], ... } */
        cJSON *category;
        cJSON_ArrayForEach(category, exposed) {
            if (cJSON_IsArray(category)) {
                cJSON *item;
                cJSON_ArrayForEach(item, category) {
                    if (cJSON_IsString(item)) {
                        DYNARRAY_PUSH(modules, modules_count, modules_capacity,
                                     arena_strdup(item->valuestring), char*);
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    arena_free(content);
    *count = modules_count;
    return modules;
}

/**
 * Convert module name (e.g., "Html.Events") to file path
 */
static char *pkg_module_name_to_path(const char *module_name, const char *src_dir) {
    int len = strlen(module_name);
    int src_len = strlen(src_dir);

    char *path = arena_malloc(src_len + 1 + len + 5);
    strcpy(path, src_dir);
    strcat(path, "/");

    char *dest = path + src_len + 1;
    for (int i = 0; i < len; i++) {
        if (module_name[i] == '.') {
            *dest++ = '/';
        } else {
            *dest++ = module_name[i];
        }
    }
    *dest = '\0';
    strcat(path, ".elm");

    return path;
}

/**
 * Recursively collect all .elm files in a directory
 */
static void pkg_collect_all_elm_files(const char *dir_path, char ***files, int *count, int *capacity) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int path_len = strlen(dir_path) + strlen(entry->d_name) + 2;
        char *full_path = arena_malloc(path_len);
        snprintf(full_path, path_len, "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                pkg_collect_all_elm_files(full_path, files, count, capacity);
            } else if (S_ISREG(st.st_mode)) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && strcmp(ext, ".elm") == 0) {
                    char *abs_path = realpath(full_path, NULL);
                    if (abs_path) {
                        DYNARRAY_PUSH(*files, *count, *capacity, arena_strdup(abs_path), char*);
                        free(abs_path);
                    }
                }
            }
        }
        arena_free(full_path);
    }

    closedir(dir);
}

/**
 * Recursively collect ALL files in a directory (for package_file facts)
 */
static void pkg_collect_all_files(const char *dir_path, char ***files, int *count, int *capacity) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int path_len = strlen(dir_path) + strlen(entry->d_name) + 2;
        char *full_path = arena_malloc(path_len);
        snprintf(full_path, path_len, "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                pkg_collect_all_files(full_path, files, count, capacity);
            } else if (S_ISREG(st.st_mode)) {
                char *abs_path = realpath(full_path, NULL);
                if (abs_path) {
                    DYNARRAY_PUSH(*files, *count, *capacity, arena_strdup(abs_path), char*);
                    free(abs_path);
                }
            }
        }
        arena_free(full_path);
    }

    closedir(dir);
}

/**
 * Find any available version of a package in a package repository.
 * Returns the path to the package directory, or NULL if not found.
 * 
 * Package repository structure:
 *   <repo_base>/packages/<author>/<name>/<version>/
 */
static char *pkg_find_any_version_in_repo(const char *repo_packages_dir, const char *author, const char *name) {
    /* Build path to author/name directory */
    size_t dir_len = strlen(repo_packages_dir) + strlen(author) + strlen(name) + 3;
    char *pkg_dir = arena_malloc(dir_len);
    if (!pkg_dir) return NULL;
    snprintf(pkg_dir, dir_len, "%s/%s/%s", repo_packages_dir, author, name);
    
    /* Open directory and find first version */
    DIR *dir = opendir(pkg_dir);
    if (!dir) {
        arena_free(pkg_dir);
        return NULL;
    }
    
    struct dirent *entry;
    char *result = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Build full path to this version */
        size_t version_path_len = strlen(pkg_dir) + strlen(entry->d_name) + 2;
        char *version_path = arena_malloc(version_path_len);
        snprintf(version_path, version_path_len, "%s/%s", pkg_dir, entry->d_name);
        
        /* Check if it's a directory with an elm.json */
        struct stat st;
        if (stat(version_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t elm_json_len = strlen(version_path) + 12;
            char *elm_json_path = arena_malloc(elm_json_len);
            snprintf(elm_json_path, elm_json_len, "%s/elm.json", version_path);
            
            if (file_exists(elm_json_path)) {
                arena_free(elm_json_path);
                result = version_path;
                break;
            }
            arena_free(elm_json_path);
        }
        arena_free(version_path);
    }
    
    closedir(dir);
    arena_free(pkg_dir);
    return result;
}

/**
 * Extract package_module facts from dependencies in a package repository.
 * This is for reviewing packages, where dependencies have version ranges
 * and we need to find actual versions in the repository.
 */
static void pkg_extract_package_module_facts(Rulr *r, const ElmJson *ej, const char *repo_packages_dir) {
    if (!ej || !repo_packages_dir) return;
    
    PackageMap *deps = ej->package_dependencies;
    if (!deps) return;
    
    for (int i = 0; i < deps->count; i++) {
        Package *pkg = &deps->packages[i];
        
        /* Find any version of this package in the repository */
        char *pkg_path = pkg_find_any_version_in_repo(repo_packages_dir, pkg->author, pkg->name);
        if (!pkg_path) continue;
        
        /* Build path to elm.json */
        size_t elm_json_len = strlen(pkg_path) + 12;
        char *elm_json_path = arena_malloc(elm_json_len);
        snprintf(elm_json_path, elm_json_len, "%s/elm.json", pkg_path);
        
        /* Parse exposed modules */
        int module_count = 0;
        char **modules = parse_package_exposed_modules(elm_json_path, &module_count);
        
        if (modules) {
            for (int m = 0; m < module_count; m++) {
                if (modules[m]) {
                    rulr_insert_fact_3s(r, "package_module", pkg->author, pkg->name, modules[m]);
                }
            }
        }
        
        arena_free(elm_json_path);
        arena_free(pkg_path);
    }
}

/**
 * Extract the "packages" directory path from a package path.
 * E.g., from "/repo/0.19.1/packages/author/name/1.0.0"
 *       returns "/repo/0.19.1/packages"
 */
static char *pkg_extract_repo_packages_dir(const char *pkg_path) {
    if (!pkg_path) return NULL;
    
    /* Look for "/packages/" in the path */
    const char *packages_marker = strstr(pkg_path, "/packages/");
    if (!packages_marker) return NULL;
    
    /* Calculate the length up to and including "/packages" */
    size_t len = (packages_marker - pkg_path) + 9; /* strlen("/packages") = 9 */
    
    char *result = arena_malloc(len + 1);
    if (!result) return NULL;
    
    strncpy(result, pkg_path, len);
    result[len] = '\0';
    
    return result;
}

/**
 * Extract module name and imports from an Elm file and insert facts
 */
static void pkg_extract_file_facts(Rulr *r, const char *file_path, const char *src_dir) {
    SkeletonModule *mod = skeleton_parse(file_path);
    if (!mod) return;

    /* Insert file_module(file, module) fact */
    if (mod->module_name) {
        rulr_insert_fact_2s(r, "file_module", file_path, mod->module_name);
    }

    /* Insert file_import(file, imported_module) facts for LOCAL imports
     * and import(module) facts for ALL imports */
    for (int i = 0; i < mod->imports_count; i++) {
        const char *module_name = mod->imports[i].module_name;
        if (!module_name) continue;
        
        /* Insert import(module) for ALL imports (used by no_unused_dependencies) */
        rulr_insert_fact_1s(r, "import", module_name);
        
        /* Check if this is a local import (file exists in src/) */
        char *module_path = pkg_module_name_to_path(module_name, src_dir);
        if (module_path && file_exists(module_path)) {
            rulr_insert_fact_2s(r, "file_import", file_path, module_name);
        }
    }

    skeleton_free(mod);
}

/* ============================================================================
 * Package subcommand implementation
 * ========================================================================== */

int cmd_review_package(int argc, char *argv[]) {
    const char *pkg_path = NULL;
    char **rule_files = NULL;
    int rule_files_count = 0;
    int rule_files_capacity = 4;
    int quiet_mode = 0;

    rule_files = arena_malloc(rule_files_capacity * sizeof(char*));
    if (!rule_files) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_package_usage();
            return 0;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet_mode = 1;
        } else if (strcmp(argv[i], "--rule") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --rule requires a path argument\n");
                return 1;
            }
            DYNARRAY_PUSH(rule_files, rule_files_count, rule_files_capacity, argv[++i], char*);
        } else if (argv[i][0] != '-') {
            if (!pkg_path) {
                pkg_path = argv[i];
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    /* Validate arguments */
    if (!pkg_path) {
        fprintf(stderr, "Error: No package path specified\n");
        print_package_usage();
        return 1;
    }

    if (rule_files_count == 0) {
        fprintf(stderr, "Error: At least one --rule file is required\n");
        print_package_usage();
        return 1;
    }

    /* Clean up the package path */
    char *clean_path = strip_trailing_slash(pkg_path);
    
    /* Check for elm.json */
    char elm_json_path[2048];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", clean_path);
    if (!file_exists(elm_json_path)) {
        if (!quiet_mode) {
            fprintf(stderr, "Error: elm.json not found at '%s'\n", elm_json_path);
        }
        return 1;
    }

    /* Build src directory path */
    char src_dir[2048];
    snprintf(src_dir, sizeof(src_dir), "%s/src", clean_path);
    
    /* Parse exposed modules */
    int exposed_count = 0;
    char **exposed_modules = pkg_parse_exposed_modules(elm_json_path, &exposed_count);
    if (!exposed_modules) {
        if (!quiet_mode) {
            fprintf(stderr, "Error: Failed to parse elm.json\n");
        }
        return 1;
    }

    /* Parse full elm.json for dependency facts */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json && !quiet_mode) {
        fprintf(stderr, "Warning: Failed to parse elm.json for dependencies\n");
    }

    /* Initialize cache for package module fact extraction */
    CacheConfig *cache = NULL;
    if (elm_json) {
        cache = cache_config_init();
    }

    /* Collect all .elm files in src */
    int all_elm_files_capacity = 256;
    int all_elm_files_count = 0;
    char **all_elm_files = arena_malloc(all_elm_files_capacity * sizeof(char*));
    pkg_collect_all_elm_files(src_dir, &all_elm_files, &all_elm_files_count, &all_elm_files_capacity);

    /* Collect ALL files in the package (for package_file facts) */
    int all_pkg_files_capacity = 256;
    int all_pkg_files_count = 0;
    char **all_pkg_files = arena_malloc(all_pkg_files_capacity * sizeof(char*));
    pkg_collect_all_files(clean_path, &all_pkg_files, &all_pkg_files_count, &all_pkg_files_capacity);

    /* Build allowed root file paths */
    char license_path[2048];
    char readme_path[2048];
    snprintf(license_path, sizeof(license_path), "%s/LICENSE", clean_path);
    snprintf(readme_path, sizeof(readme_path), "%s/README.md", clean_path);
    
    /* Get absolute paths for allowed root files */
    char *abs_license = realpath(license_path, NULL);
    char *abs_readme = realpath(readme_path, NULL);
    char *abs_elm_json = realpath(elm_json_path, NULL);

    if (!quiet_mode) {
        printf("Reviewing package: %s\n", clean_path);
        printf("Exposed modules: %d\n", exposed_count);
        printf("Source files: %d\n", all_elm_files_count);
        printf("Total package files: %d\n", all_pkg_files_count);
        printf("Rule files: %d\n", rule_files_count);
        printf("\n");
    }

    /* Initialize rulr engine ONCE */
    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        if (!quiet_mode) {
            fprintf(stderr, "Error: Failed to initialize rulr engine: %s\n", err.message);
        }
        if (abs_license) free(abs_license);
        if (abs_readme) free(abs_readme);
        if (abs_elm_json) free(abs_elm_json);
        return 1;
    }

    /* Insert all facts ONCE (they will be reused across rule files) */
    
    /* Insert exposed_module(module) facts */
    for (int i = 0; i < exposed_count; i++) {
        rulr_insert_fact_1s(&rulr, "exposed_module", exposed_modules[i]);
    }

    /* Insert source_file(file) facts and extract file info from .elm files */
    for (int i = 0; i < all_elm_files_count; i++) {
        rulr_insert_fact_1s(&rulr, "source_file", all_elm_files[i]);
        pkg_extract_file_facts(&rulr, all_elm_files[i], src_dir);
    }

    /* Insert package_file facts for ALL files in the package
     * We inject multiple facts for flexibility in writing rules:
     * - package_file(abs_path) - absolute path (for error reporting)
     * - package_file_rel(rel_path) - path relative to package root
     * - package_file_name(filename) - just the filename
     * - package_file_info(abs_path, rel_path, filename) - all three together
     */
    size_t clean_path_len = strlen(clean_path);
    for (int i = 0; i < all_pkg_files_count; i++) {
        const char *abs_path = all_pkg_files[i];
        rulr_insert_fact_1s(&rulr, "package_file", abs_path);
        
        /* Calculate relative path */
        const char *rel_path = abs_path;
        if (strncmp(abs_path, clean_path, clean_path_len) == 0 && 
            abs_path[clean_path_len] == '/') {
            rel_path = abs_path + clean_path_len + 1;
        }
        rulr_insert_fact_1s(&rulr, "package_file_rel", rel_path);
        
        /* Extract filename */
        const char *filename = strrchr(abs_path, '/');
        filename = filename ? filename + 1 : abs_path;
        rulr_insert_fact_1s(&rulr, "package_file_name", filename);
        
        /* Insert combined fact for joins */
        rulr_insert_fact_3s(&rulr, "package_file_info", abs_path, rel_path, filename);
    }

    /* Insert allowed_root_file facts for LICENSE, README.md, elm.json */
    if (abs_license) {
        rulr_insert_fact_1s(&rulr, "allowed_root_file", abs_license);
    }
    if (abs_readme) {
        rulr_insert_fact_1s(&rulr, "allowed_root_file", abs_readme);
    }
    if (abs_elm_json) {
        rulr_insert_fact_1s(&rulr, "allowed_root_file", abs_elm_json);
    }

    /* Insert elm.json facts (including dependency facts) */
    if (elm_json) {
        extract_elm_json_facts(&rulr, elm_json);
        
        /* Extract package_module facts from package repository
         * (for packages, dependencies are in the same repository) */
        char *repo_packages_dir = pkg_extract_repo_packages_dir(clean_path);
        if (repo_packages_dir) {
            pkg_extract_package_module_facts(&rulr, elm_json, repo_packages_dir);
            arena_free(repo_packages_dir);
        } else if (cache) {
            /* Fall back to ELM_HOME cache if not in a package repository */
            extract_package_module_facts(&rulr, elm_json, cache);
        }
    }

    /* Run each rule file, reusing the injected facts AND derived predicates.
     * Derived predicates from earlier rule files are preserved for use by
     * later rule files. Use clear_derived() in a rule file if you need
     * to reset derived predicates between files. */
    int total_errors = 0;
    for (int r = 0; r < rule_files_count; r++) {
        const char *rule_path = rule_files[r];

        if (!quiet_mode) {
            printf("=== Rule file: %s ===\n", rule_path);
        }

        /* Load the rule file (tries compiled .dlc first, then source .dl) */
        err = rulr_load_rule_file(&rulr, rule_path);
        if (err.is_error) {
            if (!quiet_mode) {
                fprintf(stderr, "Error: Failed to load rule file: %s\n", err.message);
            }
            continue;
        }

        /* Evaluate the rules */
        err = rulr_evaluate(&rulr);
        if (err.is_error) {
            if (!quiet_mode) {
                fprintf(stderr, "Error: Rule evaluation failed: %s\n", err.message);
            }
            continue;
        }

        /* Get and print the 'error' relation */
        EngineRelationView error_view = rulr_get_relation(&rulr, "error");
        EngineRelationView redundant_view = rulr_get_relation(&rulr, "redundant_file");
        
        /* Check if error and redundant_file have the same count - if so, they're likely identical
         * and we should only print the more descriptive redundant_file section */
        int skip_error_detail = (error_view.pred_id >= 0 && redundant_view.pred_id >= 0 &&
                                  error_view.num_tuples == redundant_view.num_tuples &&
                                  error_view.num_tuples > 0);
        
        if (error_view.pred_id >= 0 && error_view.num_tuples > 0) {
            total_errors += error_view.num_tuples;
            if (quiet_mode) {
                /* In quiet mode, bail on first error with exit code 100 */
                rulr_deinit(&rulr);
                if (elm_json) elm_json_free(elm_json);
                if (cache) cache_config_free(cache);
                if (abs_license) free(abs_license);
                if (abs_readme) free(abs_readme);
                if (abs_elm_json) free(abs_elm_json);
                return 100;
            }
            if (skip_error_detail) {
                printf("Found %d error(s) (see redundant files below)\n", error_view.num_tuples);
            } else {
                printf("Found %d error(s):\n", error_view.num_tuples);
                reporter_print_errors(&rulr, error_view, clean_path);
            }
        } else if (!quiet_mode) {
            printf("No errors found.\n");
        }

        /* Also check for 'warning' relation */
        EngineRelationView warning_view = rulr_get_relation(&rulr, "warning");
        if (!quiet_mode && warning_view.pred_id >= 0 && warning_view.num_tuples > 0) {
            printf("Found %d warning(s):\n", warning_view.num_tuples);
            print_relation("warning", &rulr, warning_view);
        }

        /* Print redundant_file relation (already obtained above) */
        if (!quiet_mode && redundant_view.pred_id >= 0 && redundant_view.num_tuples > 0) {
            printf("\n⚠️  Redundant files (%d):\n", redundant_view.num_tuples);
            reporter_print_redundant_files(&rulr, redundant_view, clean_path);
        }

        if (!quiet_mode) {
            printf("\n");
        }
    }

    /* Cleanup */
    rulr_deinit(&rulr);
    if (elm_json) {
        elm_json_free(elm_json);
    }
    if (cache) {
        cache_config_free(cache);
    }
    if (abs_license) free(abs_license);
    if (abs_readme) free(abs_readme);
    if (abs_elm_json) free(abs_elm_json);

    if (!quiet_mode) {
        printf("Total errors: %d\n", total_errors);
    }
    return total_errors > 0 ? 1 : 0;
}

/* ============================================================================
 * Main entry point
 * ========================================================================== */

int cmd_review(int argc, char *argv[]) {
    if (argc < 2) {
        print_review_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_review_usage();
        return 0;
    }

    if (strcmp(subcmd, "file") == 0) {
        return cmd_review_file(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "package") == 0) {
        return cmd_review_package(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown review subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s review --help' for usage information.\n", program_name);
    return 1;
}
