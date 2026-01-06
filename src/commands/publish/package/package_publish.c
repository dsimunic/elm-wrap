/**
 * package_publish.c - Package prepublish command implementation
 *
 * Uses rulr (Datalog) rules to determine which files should be included
 * when publishing a package. This ensures the publish logic stays in sync
 * with validation rules like no_redundant_files.dl.
 */

#include "package_publish.h"
#include "../../review/reporter.h"
#include "../../../alloc.h"
#include "../../../constants.h"
#include "../../../global_context.h"
#include "../../../elm_json.h"
#include "../../../fileutil.h"
#include "../../../plural.h"
#include "../../../messages.h"
#include "../../../shared/log.h"
#include "../../wrappers/elm_cmd_common.h"
#include "../../wrappers/builder.h"
#include "../../../install_env.h"
#include "../../../elm_compiler.h"
#include "../../../ast/skeleton.h"
#include "../../../dyn_array.h"
#include "../../../vendor/cJSON.h"
#include "../docs/elm_docs.h"
#include "../docs/docs_json.h"
#include "../../../cache.h"
#include "../docs/dependency_cache.h"
#include "../docs/path_util.h"
#include "rulr.h"
#include "../../../rulr/rulr_dl.h"
#include "../../../rulr/host_helpers.h"
#include "engine/engine.h"
#include "runtime/runtime.h"
#include "common/types.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* ============================================================================
 * Usage
 * ========================================================================== */

static void print_usage(void) {
    user_message("Usage: %s package prepublish SOURCE_PATH [OPTIONS]\n", global_context_program_name());
    user_message("\n");
    user_message("Determine which files would be published from a package.\n");
    user_message("\n");
    user_message("Arguments:\n");
    user_message("  SOURCE_PATH           Path to the package directory\n");
    user_message("\n");
    user_message("Options:\n");
    user_message("  -h, --help            Show this help message\n");
    user_message("  --delete-extra        Offer to delete files that would NOT be published\n");
    user_message("  --git-exclude-extras  Generate .gitattributes to exclude extras from git archive\n");
    user_message("  --generate-docs-json  Generate docs.json file in package root\n");
    user_message("  -f, --overwrite       Overwrite existing docs.json (use with --generate-docs-json)\n");
    user_message("\n");
}

/* ============================================================================
 * File collection helpers
 * ========================================================================== */

static char **parse_exposed_modules(const char *elm_json_path, int *count) {
    char *content = file_read_contents_bounded(elm_json_path, MAX_ELM_JSON_FILE_BYTES, NULL);
    if (!content) return NULL;

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        arena_free(content);
        return NULL;
    }

    int modules_capacity = INITIAL_MODULE_CAPACITY;
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

static char *parse_string_field_from_elm_json(const char *elm_json_path, const char *field_name) {
    char *content = file_read_contents_bounded(elm_json_path, MAX_ELM_JSON_FILE_BYTES, NULL);
    if (!content) return NULL;

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        arena_free(content);
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItem(root, field_name);
    char *result = NULL;
    if (item && cJSON_IsString(item) && item->valuestring) {
        result = arena_strdup(item->valuestring);
    }

    cJSON_Delete(root);
    arena_free(content);
    return result;
}

static char *module_name_to_path(const char *module_name, const char *src_dir) {
    if (!module_name || !src_dir) return NULL;

    size_t module_len = strlen(module_name);
    size_t src_len = strlen(src_dir);
    size_t required = src_len + 1 + module_len + 4 + 1;
    if (required > MAX_PATH_LENGTH) return NULL;

    char *path = arena_malloc(required);
    if (!path) return NULL;

    memcpy(path, src_dir, src_len);
    path[src_len] = '/';

    char *dest = path + src_len + 1;
    for (size_t i = 0; i < module_len; i++) {
        *dest++ = (module_name[i] == '.') ? '/' : module_name[i];
    }
    memcpy(dest, ".elm", 5);
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
                    char abs_path[MAX_PATH_LENGTH];
                    if (realpath(full_path, abs_path)) {
                        DYNARRAY_PUSH(*files, *count, *capacity, arena_strdup(abs_path), char*);
                    }
                }
            }
        }
        arena_free(full_path);
    }

    closedir(dir);
}

static bool string_list_contains(const char **items, int count, const char *needle) {
    if (!items || count <= 0 || !needle) return false;
    for (int i = 0; i < count; i++) {
        if (items[i] && strcmp(items[i], needle) == 0) {
            return true;
        }
    }
    return false;
}

static char **load_dont_descend_into_names(int *out_count) {
    if (out_count) *out_count = 0;

    Rulr policy = {0};
    RulrError err = rulr_init(&policy);
    if (err.is_error) {
        return NULL;
    }

    err = rulr_load_rule_file(&policy, "core_package_files");
    if (err.is_error) {
        rulr_deinit(&policy);
        return NULL;
    }

    err = rulr_evaluate(&policy);
    if (err.is_error) {
        rulr_deinit(&policy);
        return NULL;
    }

    EngineRelationView view = rulr_get_relation(&policy, "dont_descend_into");
    if (view.pred_id < 0 || view.num_tuples <= 0 || !view.tuples) {
        rulr_deinit(&policy);
        return NULL;
    }

    int names_capacity = view.num_tuples;
    int names_count = 0;
    char **names = arena_malloc(names_capacity * sizeof(char*));

    const Tuple *tuples = (const Tuple *)view.tuples;
    for (int i = 0; i < view.num_tuples; i++) {
        const Tuple *t = &tuples[i];
        if (t->arity >= 1 && t->fields[0].kind == VAL_SYM) {
            const char *name = rulr_lookup_symbol(&policy, t->fields[0].u.sym);
            if (name) {
                DYNARRAY_PUSH(names, names_count, names_capacity, arena_strdup(name), char*);
            }
        }
    }

    rulr_deinit(&policy);
    if (out_count) *out_count = names_count;
    return names;
}

static void collect_all_files_with_skipped_dirs(
    const char *dir_path,
    char ***files,
    int *count,
    int *capacity,
    const char **dont_descend_names,
    int dont_descend_count,
    char ***skipped_dirs,
    int *skipped_count,
    int *skipped_capacity
) {
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
                if (string_list_contains(dont_descend_names, dont_descend_count, entry->d_name)) {
                    char abs_path[MAX_PATH_LENGTH];
                    if (realpath(full_path, abs_path)) {
                        DYNARRAY_PUSH(*skipped_dirs, *skipped_count, *skipped_capacity, arena_strdup(abs_path), char*);
                    }
                } else {
                    collect_all_files_with_skipped_dirs(full_path, files, count, capacity,
                                                        dont_descend_names, dont_descend_count,
                                                        skipped_dirs, skipped_count, skipped_capacity);
                }
            } else if (S_ISREG(st.st_mode)) {
                char abs_path[MAX_PATH_LENGTH];
                if (realpath(full_path, abs_path)) {
                    DYNARRAY_PUSH(*files, *count, *capacity, arena_strdup(abs_path), char*);
                }
            }
        }
        arena_free(full_path);
    }

    closedir(dir);
}

static bool path_is_under_dir(const char *abs_path, const char *abs_dir) {
    if (!abs_path || !abs_dir) return false;
    size_t dir_len = strlen(abs_dir);
    if (strncmp(abs_path, abs_dir, dir_len) != 0) return false;
    return abs_path[dir_len] == '/' || abs_path[dir_len] == '\0';
}

static bool prompt_yes_no_default_yes(const char *prompt) {
    user_message("%s", prompt);
    fflush(stderr);

    int c = getchar();
    if (c == '\n' || c == 'Y' || c == 'y') {
        return true;
    }
    if (c == 'N' || c == 'n') {
        while (c != '\n' && c != EOF) c = getchar();
        return false;
    }
    while (c != '\n' && c != EOF) c = getchar();
    return false;
}

/* Silent build helpers live in elm_cmd_common.c */

/* ============================================================================
 * Docs generation helpers
 * ========================================================================== */

static int compare_modules_by_name(const void *a, const void *b) {
    const ElmModuleDocs *mod_a = (const ElmModuleDocs *)a;
    const ElmModuleDocs *mod_b = (const ElmModuleDocs *)b;
    return strcmp(mod_a->name, mod_b->name);
}

static int generate_docs_json_file(
    const char *pkg_path,
    char **exposed_modules,
    int exposed_count,
    const char *output_path
) {
    char src_path[2048];
    snprintf(src_path, sizeof(src_path), "%s/src", pkg_path);

    /* Initialize dependency cache */
    CacheConfig *cache_config = cache_config_init();
    DependencyCache *dep_cache = NULL;
    if (cache_config && cache_config->elm_home) {
        dep_cache = dependency_cache_create(cache_config->elm_home, pkg_path);
    }

    /* Allocate array for all module docs */
    ElmModuleDocs *all_docs = arena_calloc(exposed_count, sizeof(ElmModuleDocs));
    if (!all_docs) {
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    /* Parse all exposed modules */
    int doc_index = 0;
    for (int i = 0; i < exposed_count; i++) {
        const char *module_name = exposed_modules[i];
        char *module_path = module_name_to_path(module_name, src_path);

        if (!module_path || !file_exists(module_path)) {
            log_warn("Exposed module '%s' not found", module_name);
            continue;
        }

        if (!parse_elm_file(module_path, &all_docs[doc_index], dep_cache)) {
            log_warn("Failed to parse module '%s'", module_name);
        } else {
            doc_index++;
        }
    }

    if (doc_index == 0) {
        log_error("No modules successfully parsed");
        arena_free(all_docs);
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    /* Sort modules alphabetically by name */
    if (doc_index > 1) {
        qsort(all_docs, doc_index, sizeof(ElmModuleDocs), compare_modules_by_name);
    }

    /* Redirect stdout to file */
    fflush(stdout);
    int stdout_fd = dup(STDOUT_FILENO);
    if (stdout_fd < 0) {
        log_error("Failed to duplicate stdout");
        arena_free(all_docs);
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    FILE *out_file = fopen(output_path, "w");
    if (!out_file) {
        log_error("Failed to open %s for writing", output_path);
        close(stdout_fd);
        arena_free(all_docs);
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    int file_fd = fileno(out_file);
    if (dup2(file_fd, STDOUT_FILENO) < 0) {
        log_error("Failed to redirect stdout");
        fclose(out_file);
        close(stdout_fd);
        arena_free(all_docs);
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    /* Generate docs.json to the redirected stdout (which is now the file) */
    print_docs_json(all_docs, doc_index);
    fflush(stdout);

    /* Restore stdout */
    dup2(stdout_fd, STDOUT_FILENO);
    close(stdout_fd);
    fclose(out_file);

    /* Cleanup */
    for (int i = 0; i < doc_index; i++) {
        free_elm_docs(&all_docs[i]);
    }
    arena_free(all_docs);

    if (dep_cache) dependency_cache_free(dep_cache);
    if (cache_config) cache_config_free(cache_config);

    return 0;
}

static void extract_file_facts(Rulr *r, const char *file_path, const char *src_dir) {
    SkeletonModule *mod = skeleton_parse(file_path);
    if (!mod) return;

    if (mod->module_name) {
        rulr_insert_fact_2s(r, "file_module", file_path, mod->module_name);
    }

    for (int i = 0; i < mod->imports_count; i++) {
        const char *module_name = mod->imports[i].module_name;
        if (!module_name) continue;
        
        char *module_path = module_name_to_path(module_name, src_dir);
        if (module_path && file_exists(module_path)) {
            rulr_insert_fact_2s(r, "file_import", file_path, module_name);
        }
    }

    skeleton_free(mod);
}

/**
 * Generate .gitattributes file to exclude extra files from git archive
 * Appends to existing .gitattributes if present
 */
static int write_gitattributes_for_extras(
    const char *pkg_root,
    const char **extra_files,
    int extra_files_count,
    const char **extra_dirs,
    int extra_dirs_count
) {
    if (!pkg_root) return 1;

    /* Build path to .gitattributes */
    char gitattributes_path[MAX_PATH_LENGTH];
    snprintf(gitattributes_path, sizeof(gitattributes_path), "%s/.gitattributes", pkg_root);

    /* Check if file already exists */
    struct stat st;
    bool file_exists = (stat(gitattributes_path, &st) == 0);

    /* Open file for appending (creates if doesn't exist) */
    FILE *f = fopen(gitattributes_path, "a");
    if (!f) {
        log_error("Failed to open %s for writing", gitattributes_path);
        return 1;
    }

    /* If file existed and has content, add a blank line separator */
    if (file_exists && st.st_size > 0) {
        fprintf(f, "\n");
    }

    /* Write header */
    fprintf(f, "# Generated by wrap package prepublish --git-exclude-extras\n");
    fprintf(f, "# These files and directories are excluded from git archive (e.g., GitHub releases)\n");
    fprintf(f, "\n");

    /* Get package root length for path stripping */
    size_t pkg_root_len = strlen(pkg_root);

    /* Write file exclusions */
    for (int i = 0; i < extra_files_count; i++) {
        const char *abs_path = extra_files[i];
        if (!abs_path) continue;

        /* Make path relative to package root */
        const char *rel_path = abs_path;
        if (strncmp(abs_path, pkg_root, pkg_root_len) == 0) {
            rel_path = abs_path + pkg_root_len;
            /* Skip leading slash */
            while (*rel_path == '/') rel_path++;
        }

        if (rel_path[0] != '\0') {
            fprintf(f, "/%s export-ignore\n", rel_path);
        }
    }

    /* Write directory exclusions */
    for (int i = 0; i < extra_dirs_count; i++) {
        const char *abs_path = extra_dirs[i];
        if (!abs_path) continue;

        /* Make path relative to package root */
        const char *rel_path = abs_path;
        if (strncmp(abs_path, pkg_root, pkg_root_len) == 0) {
            rel_path = abs_path + pkg_root_len;
            /* Skip leading slash */
            while (*rel_path == '/') rel_path++;
        }

        if (rel_path[0] != '\0') {
            fprintf(f, "/%s export-ignore\n", rel_path);
        }
    }

    fclose(f);
    return 0;
}

/* ============================================================================
 * Main command implementation
 * ========================================================================== */

int cmd_package_prepublish(int argc, char *argv[]) {
    const char *pkg_path = NULL;
    bool delete_extra = false;
    bool git_exclude_extras = false;
    bool generate_docs = false;
    bool overwrite_docs = false;
    const char *pkg_path_for_display = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "--delete-extra") == 0) {
            delete_extra = true;
        } else if (strcmp(argv[i], "--git-exclude-extras") == 0) {
            git_exclude_extras = true;
        } else if (strcmp(argv[i], "--generate-docs-json") == 0) {
            generate_docs = true;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--overwrite") == 0) {
            overwrite_docs = true;
        } else if (argv[i][0] != '-') {
            if (!pkg_path) {
                pkg_path = argv[i];
                pkg_path_for_display = argv[i];
            } else {
                log_error("Unexpected argument '%s'", argv[i]);
                return 1;
            }
        } else {
            log_error("Unknown option '%s'", argv[i]);
            return 1;
        }
    }

    if (!pkg_path) {
        log_error("No package path specified");
        print_usage();
        return 1;
    }

    if (delete_extra && git_exclude_extras) {
        log_error("--delete-extra and --git-exclude-extras are mutually exclusive");
        user_message("You can either delete files or exclude them from git archives, not both.\n");
        return 1;
    }

    /* Clean up the package path */
    char *clean_path = strip_trailing_slash(pkg_path);

    /* Resolve absolute package path for consistent comparisons */
    char clean_abs[MAX_PATH_LENGTH];
    if (!realpath(clean_path, clean_abs)) {
        log_error("Failed to resolve package path '%s'", clean_path);
        return 1;
    }
    
    /* Check for elm.json */
    char elm_json_path[2048];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", clean_abs);
    if (!file_exists(elm_json_path)) {
        log_error("elm.json not found at '%s'", elm_json_path);
        return 1;
    }

    /* Build src directory path */
    char src_dir[2048];
    snprintf(src_dir, sizeof(src_dir), "%s/src", clean_abs);

    /* Read structured elm.json (for package name/version/deps) */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json || elm_json->type != ELM_PROJECT_PACKAGE) {
        log_error("elm.json at '%s' is not a package elm.json", elm_json_path);
        if (elm_json) elm_json_free(elm_json);
        return 1;
    }

    /* Parse exposed modules */
    int exposed_count = 0;
    char **exposed_modules = parse_exposed_modules(elm_json_path, &exposed_count);
    if (!exposed_modules) {
        log_error("Failed to parse elm.json");
        elm_json_free(elm_json);
        return 1;
    }

    char *license_str = parse_string_field_from_elm_json(elm_json_path, "license");

    /* Run a silent build (compiler --json) and clean elm-stuff afterwards */
    char *compiler_stdout = NULL;
    bool compile_ok = elm_cmd_run_silent_package_build(clean_abs, elm_json_path,
                                                       exposed_modules, exposed_count,
                                                       &compiler_stdout);

    /* Force delete elm-stuff if it exists (should have been deleted by silent compile, but make sure) */
    char elm_stuff_path[MAX_PATH_LENGTH];
    snprintf(elm_stuff_path, sizeof(elm_stuff_path), "%s/elm-stuff", clean_abs);
    struct stat elm_stuff_st;
    if (stat(elm_stuff_path, &elm_stuff_st) == 0) {
        remove_directory_recursive(elm_stuff_path);
    }

    /* Collect all .elm files in src */
    int all_elm_files_capacity = 256;
    int all_elm_files_count = 0;
    char **all_elm_files = arena_malloc(all_elm_files_capacity * sizeof(char*));
    collect_all_elm_files(src_dir, &all_elm_files, &all_elm_files_count, &all_elm_files_capacity);

    /* Ask policy which directory names we should not descend into */
    int dont_descend_count = 0;
    char **dont_descend_names = load_dont_descend_into_names(&dont_descend_count);

    /* Collect ALL files in the package (but do not descend into policy-defined dirs) */
    int all_pkg_files_capacity = 256;
    int all_pkg_files_count = 0;
    char **all_pkg_files = arena_malloc(all_pkg_files_capacity * sizeof(char*));

    int skipped_dirs_capacity = INITIAL_SMALL_CAPACITY;
    int skipped_dirs_count = 0;
    char **skipped_dirs = arena_malloc(skipped_dirs_capacity * sizeof(char*));

    collect_all_files_with_skipped_dirs(clean_abs,
                                        &all_pkg_files, &all_pkg_files_count, &all_pkg_files_capacity,
                                        (const char **)dont_descend_names, dont_descend_count,
                                        &skipped_dirs, &skipped_dirs_count, &skipped_dirs_capacity);

    /* Build allowed root file paths */
    char license_path[2048];
    char readme_path[2048];
    snprintf(license_path, sizeof(license_path), "%s/LICENSE", clean_abs);
    snprintf(readme_path, sizeof(readme_path), "%s/README.md", clean_abs);
    
    char *abs_license = NULL;
    char *abs_readme = NULL;
    char *abs_elm_json = NULL;

    {
        char resolved[MAX_PATH_LENGTH];
        if (realpath(license_path, resolved)) abs_license = arena_strdup(resolved);
        if (realpath(readme_path, resolved)) abs_readme = arena_strdup(resolved);
        if (realpath(elm_json_path, resolved)) abs_elm_json = arena_strdup(resolved);
    }

    /* Initialize rulr engine */
    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        log_error("Failed to initialize rulr engine: %s", err.message);
        if (abs_license) arena_free(abs_license);
        if (abs_readme) arena_free(abs_readme);
        if (abs_elm_json) arena_free(abs_elm_json);
        return 1;
    }

    /* Insert exposed_module facts */
    for (int i = 0; i < exposed_count; i++) {
        rulr_insert_fact_1s(&rulr, "exposed_module", exposed_modules[i]);
    }

    /* Facts needed by no_invalid_package_layout */
    rulr_insert_fact_1s(&rulr, "project_type", "package");
    if (elm_json->package_name) {
        rulr_insert_fact_1s(&rulr, "package_name", elm_json->package_name);
    } else {
        rulr_insert_fact_1s(&rulr, "package_name", "");
    }

    /* Insert source_file facts and extract file_module/file_import */
    for (int i = 0; i < all_elm_files_count; i++) {
        rulr_insert_fact_1s(&rulr, "source_file", all_elm_files[i]);
        extract_file_facts(&rulr, all_elm_files[i], src_dir);
    }

    /* Insert package_file_info facts */
    size_t clean_path_len = strlen(clean_abs);
    for (int i = 0; i < all_pkg_files_count; i++) {
        const char *abs_path = all_pkg_files[i];
        
        const char *rel_path = abs_path;
        if (strncmp(abs_path, clean_abs, clean_path_len) == 0 && 
            abs_path[clean_path_len] == '/') {
            rel_path = abs_path + clean_path_len + 1;
        }
        
        const char *filename = strrchr(abs_path, '/');
        filename = filename ? filename + 1 : abs_path;
        
        rulr_insert_fact_3s(&rulr, "package_file_info", abs_path, rel_path, filename);
    }

    /* Insert allowed_root_file facts */
    if (abs_license) {
        rulr_insert_fact_1s(&rulr, "allowed_root_file", abs_license);
    }
    if (abs_readme) {
        rulr_insert_fact_1s(&rulr, "allowed_root_file", abs_readme);
    }
    if (abs_elm_json) {
        rulr_insert_fact_1s(&rulr, "allowed_root_file", abs_elm_json);
    }

    /* Load rule files: use separate rule files that share derived predicates.
     * Uses rulr_load_rule_file which tries .dlc first, then .dl */
    const char *rule_paths[] = {
        "core_package_files",
        "publish_files",
        "no_invalid_package_layout",
        NULL
    };

    for (int i = 0; rule_paths[i] != NULL; i++) {
        err = rulr_load_rule_file(&rulr, rule_paths[i]);
        if (err.is_error) {
            log_error("Failed to load rule file '%s': %s", rule_paths[i], err.message);
            rulr_deinit(&rulr);
            if (abs_license) arena_free(abs_license);
            if (abs_readme) arena_free(abs_readme);
            if (abs_elm_json) arena_free(abs_elm_json);
            return 1;
        }
    }

    /* Evaluate the rules */
    err = rulr_evaluate(&rulr);
    if (err.is_error) {
        log_error("Rule evaluation failed: %s", err.message);
        rulr_deinit(&rulr);
        if (abs_license) arena_free(abs_license);
        if (abs_readme) arena_free(abs_readme);
        if (abs_elm_json) arena_free(abs_elm_json);
        return 1;
    }

    /* Get the publish_file relation */
    EngineRelationView publish_view = rulr_get_relation(&rulr, "publish_file");
    
    if (publish_view.pred_id < 0 || publish_view.num_tuples == 0) {
        user_message("No files to publish.\n");
        rulr_deinit(&rulr);
        if (abs_license) arena_free(abs_license);
        if (abs_readme) arena_free(abs_readme);
        if (abs_elm_json) arena_free(abs_elm_json);
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
    const char *pkg_name = (elm_json->package_name ? elm_json->package_name : "(unknown)");
    const char *pkg_version = (elm_json->package_version ? elm_json->package_version : "(unknown)");
    const char *license_to_show = (license_str ? license_str : "(unknown)");

    user_message("Publishing %s@%s includes the following %d %s:\n\n",
           pkg_name,
           pkg_version,
           paths_count,
           en_plural_s((long)paths_count, "file", "files"));

    /* Print included file tree, but show a short header like wrap extract */
    const char *display_root = (pkg_path_for_display ? pkg_path_for_display : clean_abs);
    display_root = strip_trailing_slash(display_root);
    user_message("  %s\n", display_root);

    ReporterConfig cfg = reporter_default_config();
    cfg.base_path = clean_abs;
    cfg.show_base = 0;
    reporter_print_file_tree(&cfg, paths, paths_count);

    user_message("\n");
    user_message("Exposing the following modules:\n\n");
    if (exposed_count == 0) {
        user_message("  (none)\n");
    } else {
        for (int i = 0; i < exposed_count; i++) {
            if (exposed_modules[i]) {
                user_message("  %s\n", exposed_modules[i]);
            }
        }
    }

    user_message("\n");
    user_message("The package has following %s:\n\n",
           en_plural_s((long)(elm_json->package_dependencies ? elm_json->package_dependencies->count : 0),
                       "dependency", "dependencies"));

    if (elm_json->package_dependencies && elm_json->package_dependencies->count > 0) {
        for (int i = 0; i < elm_json->package_dependencies->count; i++) {
            Package *p = &elm_json->package_dependencies->packages[i];
            if (!p->author || !p->name || !p->version) continue;
            user_message("  %s/%s: %s\n", p->author, p->name, p->version);
        }
    } else {
        user_message("  (none)\n");
    }

    user_message("\n");
    user_message("and will be published under %s license.\n\n", license_to_show);

    /* Report invalid package layouts (missing mandatory files, etc.) */
    EngineRelationView error_view = rulr_get_relation(&rulr, "error");
    if (error_view.pred_id >= 0 && error_view.num_tuples > 0) {
        user_message("The package has the following layout issues:\n\n");
        const Tuple *error_tuples = (const Tuple *)error_view.tuples;
        for (int i = 0; i < error_view.num_tuples; i++) {
            const Tuple *t = &error_tuples[i];
            if (t->arity >= 1 && t->fields[0].kind == VAL_SYM) {
                const char *msg = rulr_lookup_symbol(&rulr, t->fields[0].u.sym);
                if (msg) {
                    user_message("  %s\n", msg);
                }
            }
        }
        user_message("\n");
    }

    if (compile_ok) {
        user_message("The package compiles successfully.\n\n");
    } else {
        char **error_paths = NULL;
        int file_count = elm_cmd_get_compiler_error_paths(compiler_stdout, &error_paths);
        if (file_count > 0) {
            user_message("The package failed to compile. There's a problem with %d %s:\n\n",
                   file_count,
                   en_plural_s((long)file_count, "file", "files"));
            for (int i = 0; i < file_count; i++) {
                char *rel = elm_cmd_path_relative_to_base(error_paths[i], clean_abs);
                user_message("  %s\n", rel ? rel : "(unknown)");
            }
            user_message("\n");
        } else {
            user_message("The package failed to compile.\n\n");
        }
    }

    /* Identify extra files via Datalog policy (extra_file relation) */
    EngineRelationView extra_view = rulr_get_relation(&rulr, "extra_file");
    int extra_count = 0;
    int extra_capacity = (extra_view.num_tuples > 0 ? extra_view.num_tuples : 0);
    const char **extra_abs = NULL;
    const char **extra_rel = NULL;

    if (extra_view.pred_id >= 0 && extra_view.num_tuples > 0) {
        extra_abs = arena_malloc(extra_capacity * sizeof(char*));
        extra_rel = arena_malloc(extra_capacity * sizeof(char*));

        const Tuple *extra_tuples = (const Tuple *)extra_view.tuples;
        for (int i = 0; i < extra_view.num_tuples; i++) {
            const Tuple *t = &extra_tuples[i];
            if (t->arity >= 2 && t->fields[0].kind == VAL_SYM && t->fields[1].kind == VAL_SYM) {
                const char *abs = rulr_lookup_symbol(&rulr, t->fields[0].u.sym);
                const char *rel = rulr_lookup_symbol(&rulr, t->fields[1].u.sym);
                if (abs && rel) {
                    extra_abs[extra_count] = arena_strdup(abs);
                    extra_rel[extra_count] = arena_strdup(rel);
                    extra_count++;
                }
            }
        }
    }

    /* Combine extra files and skipped directories into a single list for display */
    int combined_extra_capacity = extra_count + skipped_dirs_count;
    int combined_extra_count = 0;
    const char **combined_extra = arena_malloc(combined_extra_capacity * sizeof(char*));

    /* Add extra files */
    for (int i = 0; i < extra_count; i++) {
        if (extra_abs && extra_abs[i]) {
            combined_extra[combined_extra_count++] = extra_abs[i];
        }
    }

    /* Add skipped directories (only if they still exist) */
    for (int i = 0; i < skipped_dirs_count; i++) {
        if (skipped_dirs[i]) {
            struct stat st;
            if (stat(skipped_dirs[i], &st) == 0) {
                combined_extra[combined_extra_count++] = skipped_dirs[i];
            }
        }
    }

    if (combined_extra_count > 0) {
        int file_count = extra_count;
        int dir_count = combined_extra_count - extra_count;

        if (file_count > 0 && dir_count > 0) {
            user_message("The directory also contains the following %d %s and %d %s that should NOT\n",
                   file_count, en_plural_s((long)file_count, "file", "files"),
                   dir_count, en_plural_s((long)dir_count, "directory", "directories"));
        } else if (file_count > 0) {
            user_message("The directory also contains the following %d %s that should NOT\n",
                   file_count, en_plural_s((long)file_count, "file", "files"));
        } else {
            user_message("The directory also contains the following %d %s that should NOT\n",
                   dir_count, en_plural_s((long)dir_count, "directory", "directories"));
        }
        user_message("be published:\n\n");

        user_message("  %s\n", display_root);

        ReporterConfig extra_cfg = reporter_default_config();
        extra_cfg.base_path = clean_abs;
        extra_cfg.show_base = 0;
        reporter_print_file_tree(&extra_cfg, combined_extra, combined_extra_count);

        user_message("\n");

        if (delete_extra && extra_count > 0) {
            if (prompt_yes_no_default_yes("Would you like me to delete them for you [Y/n] ")) {
                int deleted = 0;
                int failed = 0;
                for (int i = 0; i < extra_count; i++) {
                    const char *abs_path = extra_abs[i];
                    if (!abs_path) continue;
                    if (!path_is_under_dir(abs_path, clean_abs)) {
                        failed++;
                        continue;
                    }
                    if (unlink(abs_path) == 0) {
                        deleted++;
                    } else {
                        failed++;
                    }
                }
                user_message("\nDeleted %d %s", deleted, en_plural_s((long)deleted, "file", "files"));
                if (failed > 0) {
                    user_message(", failed to delete %d %s", failed, en_plural_s((long)failed, "file", "files"));
                }
                user_message(".\n");
            }
        }

        if (git_exclude_extras && (extra_count > 0 || skipped_dirs_count > 0)) {
            /* Check if .gitattributes already exists */
            char gitattributes_path[MAX_PATH_LENGTH];
            snprintf(gitattributes_path, sizeof(gitattributes_path), "%s/.gitattributes", clean_abs);
            struct stat gitattr_st;
            bool gitattr_exists = (stat(gitattributes_path, &gitattr_st) == 0);

            user_message("\n%s .gitattributes to exclude extras from git archive...\n",
                       gitattr_exists ? "Updating" : "Generating");
            int result = write_gitattributes_for_extras(clean_abs,
                                                        (const char **)extra_abs, extra_count,
                                                        (const char **)skipped_dirs, skipped_dirs_count);
            if (result == 0) {
                user_message("Successfully %s %s/.gitattributes\n",
                           gitattr_exists ? "updated" : "created", clean_abs);
                user_message("Added exclusions for %d %s and %d %s.\n\n",
                           extra_count, en_plural_s((long)extra_count, "file", "files"),
                           skipped_dirs_count, en_plural_s((long)skipped_dirs_count, "directory", "directories"));
            } else {
                log_error("Failed to generate .gitattributes");
            }
        }
    }

    /* Generate docs.json if requested */
    if (generate_docs) {
        char docs_json_path[2048];
        snprintf(docs_json_path, sizeof(docs_json_path), "%s/docs.json", clean_abs);

        struct stat st;
        bool docs_exists = (stat(docs_json_path, &st) == 0);

        if (docs_exists && !overwrite_docs) {
            user_message("You asked to generate a docs.json file, but one already exists!\n");
            user_message("I did not overwrite it. If you want to overwrite, specify `-f` or `--overwrite` together with `--generate-docs-json`.\n\n");
        } else {
            user_message("Generating docs.json...\n");
            int docs_result = generate_docs_json_file(clean_abs, exposed_modules, exposed_count, docs_json_path);
            if (docs_result == 0) {
                user_message("Successfully generated %s\n\n", docs_json_path);
            } else {
                log_error("Failed to generate docs.json");
                user_message("\n");
            }
        }
    }

    /* Final cleanup - always delete elm-stuff before returning */
    {
        char final_elm_stuff[MAX_PATH_LENGTH];
        snprintf(final_elm_stuff, sizeof(final_elm_stuff), "%s/elm-stuff", clean_abs);
        struct stat st;
        if (stat(final_elm_stuff, &st) == 0) {
            remove_directory_recursive(final_elm_stuff);
        }
    }

    /* Cleanup */
    rulr_deinit(&rulr);
    if (abs_license) arena_free(abs_license);
    if (abs_readme) arena_free(abs_readme);
    if (abs_elm_json) arena_free(abs_elm_json);

    if (license_str) arena_free(license_str);
    if (elm_json) elm_json_free(elm_json);

    return 0;
}
