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
#include "../../alloc.h"
#include "../../progname.h"
#include "../../elm_json.h"
#include "../../ast/skeleton.h"
#include "../../ast/util.h"
#include "../../dyn_array.h"
#include "../../cache.h"
#include "../../cJSON.h"
#include "../../rulr/rulr.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/engine/engine.h"
#include "../../rulr/runtime/runtime.h"
#include "../../rulr/common/types.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

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
    printf("\n");
    printf("Options:\n");
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
    printf("  --rule <PATH>      Path to a rulr rule file (.dl) - can be repeated\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s review file src/Main.elm --rule rules/no-debug.dl\n", program_name);
    printf("  %s review file src/Main.elm --config elm.json --rule a.dl --rule b.dl\n", program_name);
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
 * Insert a fact with a single symbol argument.
 */
static int insert_fact_1s(Rulr *r, const char *pred, const char *s1) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        /* Register predicate if not found */
        EngineArgType types[] = {ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 1, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    if (sym1 < 0) return -1;

    Value vals[1];
    vals[0] = make_sym_value(sym1);
    return engine_insert_fact(r->engine, pid, 1, vals);
}

/**
 * Insert a fact with two symbol arguments.
 */
static int insert_fact_2s(Rulr *r, const char *pred, const char *s1, const char *s2) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        EngineArgType types[] = {ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 2, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    int sym2 = rulr_intern_symbol(r, s2);
    if (sym1 < 0 || sym2 < 0) return -1;

    Value vals[2];
    vals[0] = make_sym_value(sym1);
    vals[1] = make_sym_value(sym2);
    return engine_insert_fact(r->engine, pid, 2, vals);
}

/**
 * Insert a fact with three symbol arguments.
 */
static int insert_fact_3s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        EngineArgType types[] = {ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 3, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    int sym2 = rulr_intern_symbol(r, s2);
    int sym3 = rulr_intern_symbol(r, s3);
    if (sym1 < 0 || sym2 < 0 || sym3 < 0) return -1;

    Value vals[3];
    vals[0] = make_sym_value(sym1);
    vals[1] = make_sym_value(sym2);
    vals[2] = make_sym_value(sym3);
    return engine_insert_fact(r->engine, pid, 3, vals);
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
                insert_fact_2s(r, "sig_uses_type", func_name, type_name);
                
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
        insert_fact_1s(r, "file_path", mod->filepath);
    }

    /* Module name fact */
    if (mod->module_name) {
        insert_fact_1s(r, "module", mod->module_name);
    }

    /* Exported values */
    if (mod->exports.expose_all) {
        /* All values are exported - mark with special symbol */
        insert_fact_1s(r, "export_all", "true");
    }
    for (int i = 0; i < mod->exports.values_count; i++) {
        insert_fact_1s(r, "exported_value", mod->exports.values[i]);
    }
    for (int i = 0; i < mod->exports.types_count; i++) {
        insert_fact_1s(r, "exported_type", mod->exports.types[i]);
    }
    for (int i = 0; i < mod->exports.types_with_constructors_count; i++) {
        insert_fact_1s(r, "exported_type_with_constructors", mod->exports.types_with_constructors[i]);
    }

    /* Imports */
    for (int i = 0; i < mod->imports_count; i++) {
        const SkeletonImport *imp = &mod->imports[i];
        if (imp->module_name) {
            insert_fact_1s(r, "import", imp->module_name);

            if (imp->alias) {
                insert_fact_2s(r, "import_alias", imp->module_name, imp->alias);
            }

            if (imp->expose_all) {
                insert_fact_2s(r, "import_expose_all", imp->module_name, "true");
            }

            for (int j = 0; j < imp->exposed_values_count; j++) {
                insert_fact_2s(r, "import_exposing", imp->module_name, imp->exposed_values[j]);
            }
            for (int j = 0; j < imp->exposed_types_count; j++) {
                insert_fact_2s(r, "import_exposing_type", imp->module_name, imp->exposed_types[j]);
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
            
            insert_fact_2s(r, "type_annotation", ann->name, type_str);
            
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
            insert_fact_1s(r, "type_alias", alias->name);
        }
    }

    /* Union types and constructors */
    for (int i = 0; i < mod->union_types_count; i++) {
        const SkeletonUnionType *ut = &mod->union_types[i];
        if (ut->name) {
            insert_fact_1s(r, "union_type", ut->name);

            for (int j = 0; j < ut->constructors_count; j++) {
                const SkeletonUnionConstructor *ctor = &ut->constructors[j];
                if (ctor->name) {
                    insert_fact_2s(r, "constructor", ut->name, ctor->name);
                }
            }
        }
    }

    /* Infix operators */
    for (int i = 0; i < mod->infixes_count; i++) {
        const SkeletonInfix *infix = &mod->infixes[i];
        if (infix->operator && infix->function_name) {
            insert_fact_2s(r, "infix", infix->operator, infix->function_name);
        }
    }
}

/* ============================================================================
 * Fact extraction from elm.json
 * ========================================================================== */

static void extract_elm_json_facts(Rulr *r, const ElmJson *ej) {
    /* Project type */
    const char *project_type = (ej->type == ELM_PROJECT_APPLICATION) ? "application" : "package";
    insert_fact_1s(r, "project_type", project_type);

    /* Elm version */
    if (ej->elm_version) {
        insert_fact_1s(r, "elm_version", ej->elm_version);
    }

    /* Package name (for packages) */
    if (ej->package_name) {
        insert_fact_1s(r, "package_name", ej->package_name);
    }
    if (ej->package_version) {
        insert_fact_1s(r, "package_version", ej->package_version);
    }

    /* Direct dependencies */
    if (ej->dependencies_direct) {
        for (int i = 0; i < ej->dependencies_direct->count; i++) {
            Package *pkg = &ej->dependencies_direct->packages[i];
            insert_fact_3s(r, "dependency", pkg->author, pkg->name, pkg->version);
        }
    }

    /* Package dependencies (for packages) */
    if (ej->package_dependencies) {
        for (int i = 0; i < ej->package_dependencies->count; i++) {
            Package *pkg = &ej->package_dependencies->packages[i];
            insert_fact_3s(r, "dependency", pkg->author, pkg->name, pkg->version);
        }
    }

    /* Indirect dependencies */
    if (ej->dependencies_indirect) {
        for (int i = 0; i < ej->dependencies_indirect->count; i++) {
            Package *pkg = &ej->dependencies_indirect->packages[i];
            insert_fact_3s(r, "indirect_dependency", pkg->author, pkg->name, pkg->version);
        }
    }

    /* Test dependencies */
    if (ej->dependencies_test_direct) {
        for (int i = 0; i < ej->dependencies_test_direct->count; i++) {
            Package *pkg = &ej->dependencies_test_direct->packages[i];
            insert_fact_3s(r, "test_dependency", pkg->author, pkg->name, pkg->version);
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
                    insert_fact_3s(r, "package_module", pkg->author, pkg->name, modules[m]);
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
            fprintf(stderr, "Warning: Failed to read elm.json at '%s'\n", config_path);
        }
    } else {
        /* Try to auto-detect elm.json */
        char *detected_path = find_elm_json(elm_file);
        if (detected_path) {
            elm_json = elm_json_read(detected_path);
            if (elm_json) {
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

    printf("Reviewing: %s\n", elm_file);
    if (mod->module_name) {
        printf("Module: %s\n", mod->module_name);
    }
    printf("\n");

    /* Run each rule file */
    int total_errors = 0;
    for (int r = 0; r < rule_files_count; r++) {
        const char *rule_path = rule_files[r];

        printf("=== Rule file: %s ===\n", rule_path);

        /* Initialize rulr engine */
        Rulr rulr;
        RulrError err = rulr_init(&rulr);
        if (err.is_error) {
            fprintf(stderr, "Error: Failed to initialize rulr engine: %s\n", err.message);
            continue;
        }

        /* Load the rule file */
        err = rulr_load_dl_file(&rulr, rule_path);
        if (err.is_error) {
            fprintf(stderr, "Error: Failed to load rule file: %s\n", err.message);
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
            fprintf(stderr, "Error: Rule evaluation failed: %s\n", err.message);
            rulr_deinit(&rulr);
            continue;
        }

        /* Get and print the 'error' relation */
        EngineRelationView error_view = rulr_get_relation(&rulr, "error");
        if (error_view.pred_id >= 0 && error_view.num_tuples > 0) {
            printf("Found %d error(s):\n", error_view.num_tuples);
            print_relation("error", &rulr, error_view);
            total_errors += error_view.num_tuples;
        } else {
            printf("No errors found.\n");
        }

        /* Also check for 'warning' relation */
        EngineRelationView warning_view = rulr_get_relation(&rulr, "warning");
        if (warning_view.pred_id >= 0 && warning_view.num_tuples > 0) {
            printf("Found %d warning(s):\n", warning_view.num_tuples);
            print_relation("warning", &rulr, warning_view);
        }

        /* Check for 'info' relation */
        EngineRelationView info_view = rulr_get_relation(&rulr, "info");
        if (info_view.pred_id >= 0 && info_view.num_tuples > 0) {
            printf("Found %d info message(s):\n", info_view.num_tuples);
            print_relation("info", &rulr, info_view);
        }

        printf("\n");
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

    printf("Total errors: %d\n", total_errors);
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

    fprintf(stderr, "Error: Unknown review subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s review --help' for usage information.\n", program_name);
    return 1;
}
