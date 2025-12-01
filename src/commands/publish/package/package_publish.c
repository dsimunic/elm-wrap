/**
 * package_publish.c - Package publish command implementation
 *
 * Uses rulr (Datalog) rules to determine which files should be included
 * when publishing a package. This ensures the publish logic stays in sync
 * with validation rules like no_redundant_files.dl.
 */

#include "package_publish.h"
#include "../../review/reporter.h"
#include "../../../alloc.h"
#include "../../../progname.h"
#include "../../../elm_json.h"
#include "../../../ast/skeleton.h"
#include "../../../dyn_array.h"
#include "../../../cJSON.h"
#include "../../../rulr/rulr.h"
#include "../../../rulr/rulr_dl.h"
#include "../../../rulr/engine/engine.h"
#include "../../../rulr/runtime/runtime.h"
#include "../../../rulr/common/types.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>

/* ============================================================================
 * Usage
 * ========================================================================== */

static void print_usage(void) {
    printf("Usage: %s package publish <source-path>\n", program_name);
    printf("\n");
    printf("Determine which files should be published from a package.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <source-path>      Path to the package directory\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("This command uses Datalog rules (core_package_files.dl, publish_files.dl)\n");
    printf("to determine which files are part of the package and should be published.\n");
}

/* ============================================================================
 * Helper functions (adapted from review.c)
 * ========================================================================== */

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

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *strip_trailing_slash(const char *path) {
    if (!path) return NULL;
    
    int len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    
    char *result = arena_malloc(len + 1);
    strncpy(result, path, len);
    result[len] = '\0';
    
    return result;
}

/* ============================================================================
 * Fact insertion helpers
 * ========================================================================== */

static int insert_fact_1s(Rulr *r, const char *pred, const char *s1) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
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

/* ============================================================================
 * File collection helpers
 * ========================================================================== */

static char **parse_exposed_modules(const char *elm_json_path, int *count) {
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

    if (cJSON_IsArray(exposed)) {
        cJSON *item;
        cJSON_ArrayForEach(item, exposed) {
            if (cJSON_IsString(item)) {
                DYNARRAY_PUSH(modules, modules_count, modules_capacity, 
                             arena_strdup(item->valuestring), char*);
            }
        }
    } else if (cJSON_IsObject(exposed)) {
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

static char *module_name_to_path(const char *module_name, const char *src_dir) {
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

static void collect_all_elm_files(const char *dir_path, char ***files, int *count, int *capacity) {
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
                collect_all_elm_files(full_path, files, count, capacity);
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

static void collect_all_files(const char *dir_path, char ***files, int *count, int *capacity) {
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
                collect_all_files(full_path, files, count, capacity);
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

static void extract_file_facts(Rulr *r, const char *file_path, const char *src_dir) {
    SkeletonModule *mod = skeleton_parse(file_path);
    if (!mod) return;

    if (mod->module_name) {
        insert_fact_2s(r, "file_module", file_path, mod->module_name);
    }

    for (int i = 0; i < mod->imports_count; i++) {
        const char *module_name = mod->imports[i].module_name;
        if (!module_name) continue;
        
        char *module_path = module_name_to_path(module_name, src_dir);
        if (module_path && file_exists(module_path)) {
            insert_fact_2s(r, "file_import", file_path, module_name);
        }
    }

    skeleton_free(mod);
}

/* ============================================================================
 * Main command implementation
 * ========================================================================== */

int cmd_package_publish(int argc, char *argv[]) {
    const char *pkg_path = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
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

    if (!pkg_path) {
        fprintf(stderr, "Error: No package path specified\n");
        print_usage();
        return 1;
    }

    /* Clean up the package path */
    char *clean_path = strip_trailing_slash(pkg_path);
    
    /* Check for elm.json */
    char elm_json_path[2048];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", clean_path);
    if (!file_exists(elm_json_path)) {
        fprintf(stderr, "Error: elm.json not found at '%s'\n", elm_json_path);
        return 1;
    }

    /* Build src directory path */
    char src_dir[2048];
    snprintf(src_dir, sizeof(src_dir), "%s/src", clean_path);

    /* Parse exposed modules */
    int exposed_count = 0;
    char **exposed_modules = parse_exposed_modules(elm_json_path, &exposed_count);
    if (!exposed_modules) {
        fprintf(stderr, "Error: Failed to parse elm.json\n");
        return 1;
    }

    /* Collect all .elm files in src */
    int all_elm_files_capacity = 256;
    int all_elm_files_count = 0;
    char **all_elm_files = arena_malloc(all_elm_files_capacity * sizeof(char*));
    collect_all_elm_files(src_dir, &all_elm_files, &all_elm_files_count, &all_elm_files_capacity);

    /* Collect ALL files in the package */
    int all_pkg_files_capacity = 256;
    int all_pkg_files_count = 0;
    char **all_pkg_files = arena_malloc(all_pkg_files_capacity * sizeof(char*));
    collect_all_files(clean_path, &all_pkg_files, &all_pkg_files_count, &all_pkg_files_capacity);

    /* Build allowed root file paths */
    char license_path[2048];
    char readme_path[2048];
    snprintf(license_path, sizeof(license_path), "%s/LICENSE", clean_path);
    snprintf(readme_path, sizeof(readme_path), "%s/README.md", clean_path);
    
    char *abs_license = realpath(license_path, NULL);
    char *abs_readme = realpath(readme_path, NULL);
    char *abs_elm_json = realpath(elm_json_path, NULL);

    /* Initialize rulr engine */
    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        fprintf(stderr, "Error: Failed to initialize rulr engine: %s\n", err.message);
        if (abs_license) free(abs_license);
        if (abs_readme) free(abs_readme);
        if (abs_elm_json) free(abs_elm_json);
        return 1;
    }

    /* Insert exposed_module facts */
    for (int i = 0; i < exposed_count; i++) {
        insert_fact_1s(&rulr, "exposed_module", exposed_modules[i]);
    }

    /* Insert source_file facts and extract file_module/file_import */
    for (int i = 0; i < all_elm_files_count; i++) {
        insert_fact_1s(&rulr, "source_file", all_elm_files[i]);
        extract_file_facts(&rulr, all_elm_files[i], src_dir);
    }

    /* Insert package_file_info facts */
    size_t clean_path_len = strlen(clean_path);
    for (int i = 0; i < all_pkg_files_count; i++) {
        const char *abs_path = all_pkg_files[i];
        
        const char *rel_path = abs_path;
        if (strncmp(abs_path, clean_path, clean_path_len) == 0 && 
            abs_path[clean_path_len] == '/') {
            rel_path = abs_path + clean_path_len + 1;
        }
        
        const char *filename = strrchr(abs_path, '/');
        filename = filename ? filename + 1 : abs_path;
        
        insert_fact_3s(&rulr, "package_file_info", abs_path, rel_path, filename);
    }

    /* Insert allowed_root_file facts */
    if (abs_license) {
        insert_fact_1s(&rulr, "allowed_root_file", abs_license);
    }
    if (abs_readme) {
        insert_fact_1s(&rulr, "allowed_root_file", abs_readme);
    }
    if (abs_elm_json) {
        insert_fact_1s(&rulr, "allowed_root_file", abs_elm_json);
    }

    /* Load rule files: use separate rule files that share derived predicates */
    const char *rule_paths[] = {
        "rulr/rules/core_package_files.dl",
        "rulr/rules/publish_files.dl",
        NULL
    };

    for (int i = 0; rule_paths[i] != NULL; i++) {
        err = rulr_load_dl_file(&rulr, rule_paths[i]);
        if (err.is_error) {
            fprintf(stderr, "Error: Failed to load rule file '%s': %s\n", rule_paths[i], err.message);
            rulr_deinit(&rulr);
            if (abs_license) free(abs_license);
            if (abs_readme) free(abs_readme);
            if (abs_elm_json) free(abs_elm_json);
            return 1;
        }
    }

    /* Evaluate the rules */
    err = rulr_evaluate(&rulr);
    if (err.is_error) {
        fprintf(stderr, "Error: Rule evaluation failed: %s\n", err.message);
        rulr_deinit(&rulr);
        if (abs_license) free(abs_license);
        if (abs_readme) free(abs_readme);
        if (abs_elm_json) free(abs_elm_json);
        return 1;
    }

    /* Get the publish_file relation */
    EngineRelationView publish_view = rulr_get_relation(&rulr, "publish_file");
    
    if (publish_view.pred_id < 0 || publish_view.num_tuples == 0) {
        printf("No files to publish.\n");
        rulr_deinit(&rulr);
        if (abs_license) free(abs_license);
        if (abs_readme) free(abs_readme);
        if (abs_elm_json) free(abs_elm_json);
        return 0;
    }

    /* Collect absolute paths from the publish_file relation */
    int paths_count = 0;
    int paths_capacity = publish_view.num_tuples;
    const char **paths = arena_malloc(paths_capacity * sizeof(char*));

    const Tuple *tuples = (const Tuple *)publish_view.tuples;
    for (int i = 0; i < publish_view.num_tuples; i++) {
        const Tuple *t = &tuples[i];
        if (t->arity >= 1 && t->fields[0].kind == VAL_SYM) {
            const char *abs = rulr_lookup_symbol(&rulr, t->fields[0].u.sym);
            if (abs) {
                DYNARRAY_PUSH(paths, paths_count, paths_capacity, arena_strdup(abs), const char*);
            }
        }
    }

    /* Print the report using common tree printer */
    printf("Will publish the following files (%d):\n", paths_count);
    ReporterConfig cfg = reporter_default_config();
    cfg.base_path = clean_path;
    reporter_print_file_tree(&cfg, paths, paths_count);

    /* Cleanup */
    rulr_deinit(&rulr);
    if (abs_license) free(abs_license);
    if (abs_readme) free(abs_readme);
    if (abs_elm_json) free(abs_elm_json);

    return 0;
}
