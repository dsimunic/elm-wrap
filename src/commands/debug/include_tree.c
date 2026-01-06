#include "debug.h"
#include "../../alloc.h"
#include "../../cache.h"
#include "../package/package_common.h"
#include "../../global_context.h"
#include "../../messages.h"
#include "../../shared/log.h"
#include "../../fileutil.h"
#include "../../dyn_array.h"
#include "../../constants.h"
#include "../../vendor/cJSON.h"
#include "../../ast/skeleton.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Forward declarations */
typedef struct ExternalModuleOwnerMap ExternalModuleOwnerMap;

static int print_file_include_tree(const char *file_path);
static int print_package_include_tree(const char *dir_path);
static void collect_imports_recursive(const char *file_path, const char *src_dir,
                                       const ExternalModuleOwnerMap *external_map,
                                       char ***visited, int *visited_count, int *visited_capacity,
                                       const char *prefix);
static char *module_name_to_path(const char *module_name, const char *src_dir);
static void collect_all_elm_files(const char *dir_path, char ***files, int *count, int *capacity);
static int is_file_in_list(const char *file, char **list, int count);
static char *find_src_dir_for_file(const char *file_path);
static char *find_elm_json_for_file(const char *file_path);
static char *read_file_content(const char *filepath);
static char **parse_exposed_modules(const char *elm_json_path, int *count);

/* Tree drawing characters (UTF-8) */
#define TREE_BRANCH "├── "
#define TREE_LAST   "└── "
#define TREE_VERT   "│   "
#define TREE_SPACE  "    "

typedef struct {
    char *module_name;
    char *package_name; /* author/name */
} ExternalModuleOwner;

struct ExternalModuleOwnerMap {
    ExternalModuleOwner *items;
    int count;
    int capacity;
};

static int compare_packages_by_name(const void *a, const void *b) {
    const Package *pa = (const Package *)a;
    const Package *pb = (const Package *)b;

    int author_cmp = strcmp(pa->author, pb->author);
    if (author_cmp != 0) return author_cmp;
    return strcmp(pa->name, pb->name);
}

static int compare_module_owner_by_module(const void *a, const void *b) {
    const ExternalModuleOwner *ma = (const ExternalModuleOwner *)a;
    const ExternalModuleOwner *mb = (const ExternalModuleOwner *)b;
    return strcmp(ma->module_name, mb->module_name);
}

static void external_module_owner_map_init(ExternalModuleOwnerMap *map) {
    if (!map) return;
    map->count = 0;
    map->capacity = INITIAL_MEDIUM_CAPACITY;
    map->items = arena_malloc(map->capacity * sizeof(ExternalModuleOwner));
}

static bool external_module_owner_map_contains_module(const ExternalModuleOwnerMap *map, const char *module_name) {
    if (!map || !map->items || !module_name) return false;
    for (int i = 0; i < map->count; i++) {
        if (map->items[i].module_name && strcmp(map->items[i].module_name, module_name) == 0) {
            return true;
        }
    }
    return false;
}

static void external_module_owner_map_add(ExternalModuleOwnerMap *map, const char *module_name, char *package_name) {
    if (!map || !module_name || !package_name) return;
    if (external_module_owner_map_contains_module(map, module_name)) return;

    DYNARRAY_ENSURE_CAPACITY(map->items, map->count, map->capacity, ExternalModuleOwner);
    map->items[map->count].module_name = arena_strdup(module_name);
    map->items[map->count].package_name = package_name;
    map->count++;
}

static void external_module_owner_map_sort(ExternalModuleOwnerMap *map) {
    if (!map || !map->items || map->count <= 1) return;
    qsort(map->items, map->count, sizeof(ExternalModuleOwner), compare_module_owner_by_module);
}

static const char *external_module_owner_map_find(const ExternalModuleOwnerMap *map, const char *module_name) {
    if (!map || !map->items || map->count == 0 || !module_name) return NULL;

    ExternalModuleOwner key;
    key.module_name = (char *)module_name;
    key.package_name = NULL;

    ExternalModuleOwner *found = bsearch(&key, map->items, map->count,
                                         sizeof(ExternalModuleOwner), compare_module_owner_by_module);
    return found ? found->package_name : NULL;
}

static bool package_list_contains(Package *pkgs, int count, const char *author, const char *name) {
    if (!pkgs || !author || !name) return false;
    for (int i = 0; i < count; i++) {
        if (pkgs[i].author && pkgs[i].name &&
            strcmp(pkgs[i].author, author) == 0 && strcmp(pkgs[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void collect_packages_from_map(PackageMap *map, Package **out_pkgs, int *out_count, int *out_capacity) {
    if (!map || !out_pkgs || !out_count || !out_capacity) return;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!pkg->author || !pkg->name || !pkg->version) continue;
        if (package_list_contains(*out_pkgs, *out_count, pkg->author, pkg->name)) continue;

        DYNARRAY_ENSURE_CAPACITY(*out_pkgs, *out_count, *out_capacity, Package);
        (*out_pkgs)[*out_count].author = arena_strdup(pkg->author);
        (*out_pkgs)[*out_count].name = arena_strdup(pkg->name);
        (*out_pkgs)[*out_count].version = arena_strdup(pkg->version);
        (*out_count)++;
    }
}

static char *format_author_name(const char *author, const char *name) {
    if (!author || !name) return NULL;
    size_t len = strlen(author) + 1 + strlen(name) + 1;
    char *out = arena_malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s/%s", author, name);
    return out;
}

static char *resolve_cached_package_version(CacheConfig *cache, const char *author, const char *name, const char *version_or_constraint) {
    if (!cache || !author || !name || !version_or_constraint) return NULL;

    if (!version_is_constraint(version_or_constraint)) {
        return arena_strdup(version_or_constraint);
    }

    VersionRange range = {0};
    if (!version_parse_constraint(version_or_constraint, &range)) {
        return NULL;
    }

    char base_dir[PATH_MAX];
    int n = snprintf(base_dir, sizeof(base_dir), "%s/%s/%s", cache->packages_dir, author, name);
    if (n < 0 || n >= (int)sizeof(base_dir)) {
        return NULL;
    }

    DIR *dir = opendir(base_dir);
    if (!dir) {
        return NULL;
    }

    bool found_any = false;
    Version best = {0};
    char *best_name = NULL;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char ver_dir[PATH_MAX];
        int vn = snprintf(ver_dir, sizeof(ver_dir), "%s/%s", base_dir, entry->d_name);
        if (vn < 0 || vn >= (int)sizeof(ver_dir)) {
            continue;
        }

        struct stat st;
        if (stat(ver_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        Version cand = {0};
        if (!version_parse_safe(entry->d_name, &cand)) {
            continue;
        }

        if (!version_in_range(&cand, &range)) {
            continue;
        }

        if (!found_any || version_compare(&cand, &best) > 0) {
            best = cand;
            best_name = entry->d_name;
            found_any = true;
        }
    }

    closedir(dir);

    return found_any && best_name ? arena_strdup(best_name) : NULL;
}

static bool build_external_module_owner_map_from_elm_json(const char *elm_json_path, ExternalModuleOwnerMap *out_map) {
    if (!elm_json_path || !out_map) return false;

    ElmJson *project = elm_json_read(elm_json_path);
    if (!project) {
        return false;
    }

    CacheConfig *cache = cache_config_init();
    if (!cache) {
        elm_json_free(project);
        return false;
    }

    int pkgs_capacity = INITIAL_MEDIUM_CAPACITY;
    int pkgs_count = 0;
    Package *pkgs = arena_malloc(pkgs_capacity * sizeof(Package));
    if (!pkgs) {
        cache_config_free(cache);
        elm_json_free(project);
        return false;
    }

    if (project->type == ELM_PROJECT_APPLICATION) {
        collect_packages_from_map(project->dependencies_direct, &pkgs, &pkgs_count, &pkgs_capacity);
        collect_packages_from_map(project->dependencies_indirect, &pkgs, &pkgs_count, &pkgs_capacity);
        collect_packages_from_map(project->dependencies_test_direct, &pkgs, &pkgs_count, &pkgs_capacity);
        collect_packages_from_map(project->dependencies_test_indirect, &pkgs, &pkgs_count, &pkgs_capacity);
    } else {
        collect_packages_from_map(project->package_dependencies, &pkgs, &pkgs_count, &pkgs_capacity);
        collect_packages_from_map(project->package_test_dependencies, &pkgs, &pkgs_count, &pkgs_capacity);
    }

    if (pkgs_count > 1) {
        qsort(pkgs, pkgs_count, sizeof(Package), compare_packages_by_name);
    }

    external_module_owner_map_init(out_map);
    if (!out_map->items) {
        cache_config_free(cache);
        elm_json_free(project);
        return false;
    }

    for (int i = 0; i < pkgs_count; i++) {
        const char *author = pkgs[i].author;
        const char *name = pkgs[i].name;
        const char *ver_or_constraint = pkgs[i].version;
        if (!author || !name || !ver_or_constraint) continue;

        char *resolved_version = resolve_cached_package_version(cache, author, name, ver_or_constraint);
        if (!resolved_version) {
            continue;
        }

        char *pkg_path = cache_get_package_path(cache, author, name, resolved_version);
        arena_free(resolved_version);
        if (!pkg_path) {
            continue;
        }

        char dep_elm_json_path[PATH_MAX];
        int pn = snprintf(dep_elm_json_path, sizeof(dep_elm_json_path), "%s/elm.json", pkg_path);
        arena_free(pkg_path);
        if (pn < 0 || pn >= (int)sizeof(dep_elm_json_path)) {
            continue;
        }

        if (!file_exists(dep_elm_json_path)) {
            continue;
        }

        int exposed_count = 0;
        char **exposed = parse_exposed_modules(dep_elm_json_path, &exposed_count);
        if (!exposed || exposed_count <= 0) {
            if (exposed) arena_free(exposed);
            continue;
        }

        char *pkg_display = format_author_name(author, name);
        if (!pkg_display) {
            for (int j = 0; j < exposed_count; j++) {
                arena_free(exposed[j]);
            }
            arena_free(exposed);
            continue;
        }

        for (int j = 0; j < exposed_count; j++) {
            if (exposed[j]) {
                external_module_owner_map_add(out_map, exposed[j], pkg_display);
                arena_free(exposed[j]);
            }
        }
        arena_free(exposed);
    }

    external_module_owner_map_sort(out_map);
    cache_config_free(cache);
    elm_json_free(project);
    return true;
}

static void print_include_tree_usage(void) {
    user_message("Usage: %s debug include-tree PATH\n", global_context_program_name());
    user_message("\n");
    user_message("Show import dependency tree for an Elm file or package at PATH.\n");
    user_message("\n");
    user_message("Arguments:\n");
    user_message("  PATH       Path to an Elm file (.elm)\n");
    user_message("             or a path to a package directory with elm.json\n");
    user_message("\n");
    user_message("For packages:\n");
    user_message("  - Shows import tree for each exposed module\n");
    user_message("  - Lists redundant files not imported by any exposed module\n");
    user_message("\n");
    user_message("Options:\n");
    user_message("  -h, --help        Show this help message\n");
}

int cmd_debug_include_tree(int argc, char *argv[]) {
    if (argc < 2) {
        print_include_tree_usage();
        return 1;
    }

    const char *path = argv[1];

    if (strcmp(path, "-h") == 0 || strcmp(path, "--help") == 0) {
        print_include_tree_usage();
        return 0;
    }

    struct stat path_stat;

    if (stat(path, &path_stat) != 0) {
        log_error("Path does not exist: %s", path);
        return 1;
    }

    if (S_ISDIR(path_stat.st_mode)) {
        return print_package_include_tree(path);
    } else if (S_ISREG(path_stat.st_mode)) {
        return print_file_include_tree(path);
    } else {
        log_error("Path is neither a file nor a directory: %s", path);
        return 1;
    }
}

/**
 * Read entire file content into allocated buffer
 */
static char *read_file_content(const char *filepath) {
    return file_read_contents_bounded(filepath, MAX_ELM_JSON_FILE_BYTES, NULL);
}

/**
 * Find the src directory for a given Elm file by looking for elm.json
 */
static char *find_src_dir_for_file(const char *file_path) {
    /* Get absolute path */
    char abs_path[MAX_PATH_LENGTH];
    if (!realpath(file_path, abs_path)) return NULL;

    char *dir = arena_strdup(abs_path);
    if (!dir) return NULL;

    /* Walk up directories looking for elm.json */
    while (1) {
        char *last_slash = strrchr(dir, '/');
        if (!last_slash || last_slash == dir) {
            break;
        }
        *last_slash = '\0';

        /* Check for elm.json */
        char elm_json_path[2048];
        snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", dir);

        if (file_exists(elm_json_path)) {
            /* Found elm.json, return src path */
            size_t dir_len = strlen(dir);
            size_t cap = dir_len + 4 + 1; /* "/src" + NUL */
            char *src_dir = arena_malloc(cap);
            if (!src_dir) return NULL;
            int n = snprintf(src_dir, cap, "%s/src", dir);
            if (n < 0 || (size_t)n >= cap) {
                return NULL;
            }
            return src_dir;
        }
    }

    return NULL;
}

static char *find_elm_json_for_file(const char *file_path) {
    char abs_path[MAX_PATH_LENGTH];
    if (!realpath(file_path, abs_path)) return NULL;

    char *dir = arena_strdup(abs_path);
    if (!dir) return NULL;

    while (1) {
        char *last_slash = strrchr(dir, '/');
        if (!last_slash || last_slash == dir) {
            break;
        }
        *last_slash = '\0';

        char elm_json_path[PATH_MAX];
        int n = snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", dir);
        if (n > 0 && n < (int)sizeof(elm_json_path) && file_exists(elm_json_path)) {
            return arena_strdup(elm_json_path);
        }
    }

    return NULL;
}

/**
 * Print include tree for a single file
 */
static int print_file_include_tree(const char *file_path) {
    /* Check file exists and is .elm */
    if (!file_exists(file_path)) {
        log_error("File does not exist: %s", file_path);
        return 1;
    }

    const char *ext = strrchr(file_path, '.');
    if (!ext || strcmp(ext, ".elm") != 0) {
        log_error("File must be an Elm file (.elm): %s", file_path);
        return 1;
    }

    /* Get absolute path */
    char abs_path[MAX_PATH_LENGTH];
    if (!realpath(file_path, abs_path)) {
        log_error("Could not resolve path: %s", file_path);
        return 1;
    }

    /* Find src directory */
    char *src_dir = find_src_dir_for_file(abs_path);
    char *elm_json_path = find_elm_json_for_file(abs_path);

    ExternalModuleOwnerMap external_map = {0};
    bool have_external_map = false;
    if (elm_json_path) {
        have_external_map = build_external_module_owner_map_from_elm_json(elm_json_path, &external_map);
    }
    if (!src_dir) {
        user_message("\n⚠️  Could not find elm.json in parent directories\n");
        user_message("   Imports from external packages will not be resolved.\n");
        /* Use file's directory as fallback */
        src_dir = arena_strdup(abs_path);
        char *last_slash = strrchr(src_dir, '/');
        if (last_slash) *last_slash = '\0';
    }

    user_message("\n📄 Import tree for: %s\n", abs_path);
    user_message("   Source directory: %s\n\n", src_dir);

    /* Track visited files to avoid cycles */
    int visited_capacity = 64;
    int visited_count = 0;
    char **visited = arena_malloc(visited_capacity * sizeof(char*));

    /* Print the root file */
    user_message("%s\n", abs_path);

    /* Recursively print imports */
    collect_imports_recursive(abs_path, src_dir,
                              have_external_map ? &external_map : NULL,
                              &visited, &visited_count, &visited_capacity, "");

    user_message("\n");
    return 0;
}

/**
 * Parse elm.json exposed-modules using cJSON
 */
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
 * Parse elm.json source-directories
 */
static char **parse_source_directories(const char *elm_json_path, int *count) {
    char *content = read_file_content(elm_json_path);
    if (!content) return NULL;

    cJSON *root = cJSON_Parse(content);
    if (!root) {
        arena_free(content);
        return NULL;
    }

    int dirs_capacity = 4;
    int dirs_count = 0;
    char **dirs = arena_malloc(dirs_capacity * sizeof(char*));

    cJSON *source_dirs = cJSON_GetObjectItem(root, "source-directories");
    if (source_dirs && cJSON_IsArray(source_dirs)) {
        cJSON *item;
        cJSON_ArrayForEach(item, source_dirs) {
            if (cJSON_IsString(item)) {
                DYNARRAY_PUSH(dirs, dirs_count, dirs_capacity,
                             arena_strdup(item->valuestring), char*);
            }
        }
    }

    cJSON_Delete(root);
    arena_free(content);
    *count = dirs_count;
    return dirs;
}

/**
 * Print include tree for a package directory
 */
static int print_package_include_tree(const char *dir_path) {
    /* Strip trailing slash from directory path */
    char *clean_dir_path = strip_trailing_slash(dir_path);
    
    /* Check for elm.json */
    char elm_json_path[PATH_MAX];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", clean_dir_path);

    if (!file_exists(elm_json_path)) {
        log_error("Directory must contain elm.json: %s", clean_dir_path);
        return 1;
    }

    /* Parse exposed modules */
    int exposed_count = 0;
    char **exposed_modules = parse_exposed_modules(elm_json_path, &exposed_count);
    if (!exposed_modules) {
        log_error("Failed to parse elm.json: %s", elm_json_path);
        return 1;
    }

    /* Parse source directories */
    int source_dir_count = 0;
    char **source_dirs = parse_source_directories(elm_json_path, &source_dir_count);
    
    /* Default to src if no source directories specified */
    char src_dir[PATH_MAX];
    if (source_dir_count > 0) {
        snprintf(src_dir, sizeof(src_dir), "%s/%s", clean_dir_path, source_dirs[0]);
    } else {
        snprintf(src_dir, sizeof(src_dir), "%s/src", clean_dir_path);
    }

    user_message("\n📦 Import tree for package: %s\n", clean_dir_path);
    user_message("   Source directory: %s\n\n", src_dir);

    ExternalModuleOwnerMap external_map = {0};
    bool have_external_map = build_external_module_owner_map_from_elm_json(elm_json_path, &external_map);

    /* Track all included files for redundancy detection */
    int included_capacity = 256;
    int included_count = 0;
    char **included_files = arena_malloc(included_capacity * sizeof(char*));

    /* Process exposed modules */
    if (exposed_count > 0) {
        user_message("📚 Exposed Modules (%d):\n\n", exposed_count);

        for (int i = 0; i < exposed_count; i++) {
            const char *module_name = exposed_modules[i];
            char *module_path = module_name_to_path(module_name, src_dir);

            if (module_path && file_exists(module_path)) {
                /* Track visited for this tree */
                int visited_capacity = 64;
                int visited_count = 0;
                char **visited = arena_malloc(visited_capacity * sizeof(char*));

                /* Get absolute path */
                char abs_path[MAX_PATH_LENGTH];
                if (realpath(module_path, abs_path)) {
                    user_message("%s (%s)\n", module_name, abs_path);

                    /* Add to included files */
                    DYNARRAY_PUSH(included_files, included_count, included_capacity,
                                 arena_strdup(abs_path), char*);

                    /* Print tree and collect all imports */
                    collect_imports_recursive(abs_path, src_dir,
                                              have_external_map ? &external_map : NULL,
                                              &visited, &visited_count,
                                              &visited_capacity, "");

                    /* Add all visited files to included list */
                    for (int j = 0; j < visited_count; j++) {
                        if (!is_file_in_list(visited[j], included_files, included_count)) {
                            DYNARRAY_PUSH(included_files, included_count, included_capacity,
                                         arena_strdup(visited[j]), char*);
                        }
                    }

                    user_message("\n");
                }
            } else {
                user_message("%s (❌ NOT FOUND: %s)\n\n", module_name, 
                       module_path ? module_path : "unknown");
            }
        }
    } else {
        user_message("⚠️  No exposed modules found in elm.json\n\n");
    }

    /* Collect all .elm files in src directory */
    int all_files_capacity = 256;
    int all_files_count = 0;
    char **all_files = arena_malloc(all_files_capacity * sizeof(char*));

    collect_all_elm_files(src_dir, &all_files, &all_files_count, &all_files_capacity);

    /* Find redundant files (not included by any exposed module) */
    user_message("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    user_message("🔍 Scanning for redundant files...\n\n");

    int redundant_count = 0;
    for (int i = 0; i < all_files_count; i++) {
        if (!is_file_in_list(all_files[i], included_files, included_count)) {
            if (redundant_count == 0) {
                user_message("⚠️  Redundant files (not imported by any exposed module):\n\n");
            }
            user_message("   • %s\n", all_files[i]);
            redundant_count++;
        }
    }

    if (redundant_count == 0) {
        user_message("✅ No redundant files found. All files are included.\n");
    } else {
        user_message("\n   Total: %d redundant file(s)\n", redundant_count);
    }

    user_message("\n");
    return 0;
}

/**
 * Recursively collect and print imports for a file
 * prefix: the string to print before the branch character (accumulates "│   " or "    ")
 */
static void collect_imports_recursive(const char *file_path, const char *src_dir,
                                       const ExternalModuleOwnerMap *external_map,
                                       char ***visited, int *visited_count, int *visited_capacity,
                                       const char *prefix) {
    /* Get absolute path for consistent comparison */
    char abs_path[MAX_PATH_LENGTH];
    if (!realpath(file_path, abs_path)) return;

    /* Check if already visited (cycle detection) */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp((*visited)[i], abs_path) == 0) {
            return; /* Already processed */
        }
    }

    /* Add to visited */
    char *current_file_abs = arena_strdup(abs_path);
    DYNARRAY_PUSH(*visited, *visited_count, *visited_capacity, current_file_abs, char*);

    /* Parse the Elm file using skeleton parser (tree-sitter based) */
    SkeletonModule *mod = skeleton_parse(file_path);
    if (!mod) return;

    /* Separate imports into local (in src dir) and external (from packages) */
    int local_names_capacity = 16;
    int local_paths_capacity = 16;
    int local_imports_count = 0;
    char **local_import_names = arena_malloc(local_names_capacity * sizeof(char*));
    char **local_import_paths = arena_malloc(local_paths_capacity * sizeof(char*));
    
    int external_imports_capacity = 16;
    int external_imports_count = 0;
    char **external_import_names = arena_malloc(external_imports_capacity * sizeof(char*));
    
    for (int i = 0; i < mod->imports_count; i++) {
        const char *module_name = mod->imports[i].module_name;
        if (!module_name) continue;
        
        char *module_path = module_name_to_path(module_name, src_dir);
        
        if (module_path && file_exists(module_path)) {
            char mod_abs_path[MAX_PATH_LENGTH];
            if (realpath(module_path, mod_abs_path)) {
                /* Skip self-references (shouldn't happen but be safe) */
                if (strcmp(mod_abs_path, current_file_abs) == 0) {
                    continue;
                }
                
                /* Add to both parallel arrays - each with its own capacity */
                DYNARRAY_ENSURE_CAPACITY(local_import_names, local_imports_count, local_names_capacity, char*);
                DYNARRAY_ENSURE_CAPACITY(local_import_paths, local_imports_count, local_paths_capacity, char*);
                local_import_names[local_imports_count] = arena_strdup(module_name);
                local_import_paths[local_imports_count] = arena_strdup(mod_abs_path);
                local_imports_count++;
            }
        } else {
            /* External import (from a package dependency) */
            DYNARRAY_PUSH(external_import_names, external_imports_count, external_imports_capacity,
                         arena_strdup(module_name), char*);
        }
    }
    
    skeleton_free(mod);

    /* Total imports for is_last calculation */
    int total_imports = local_imports_count + external_imports_count;
    int current_index = 0;

    /* Print local imports first (with recursion) */
    for (int i = 0; i < local_imports_count; i++) {
        const char *module_name = local_import_names[i];
        const char *mod_abs_path = local_import_paths[i];
        
        /* Check if already visited before printing */
        int already_visited = 0;
        for (int j = 0; j < *visited_count; j++) {
            if (strcmp((*visited)[j], mod_abs_path) == 0) {
                already_visited = 1;
                break;
            }
        }

        int is_last = (current_index == total_imports - 1);
        current_index++;
        
        /* Print current prefix + branch character */
        user_message("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);

        if (already_visited) {
            user_message("%s (↩ already shown)\n", module_name);
        } else {
            user_message("%s\n", module_name);
            
            /* Build new prefix for children */
            size_t prefix_len = strlen(prefix);
            const char *suffix = is_last ? TREE_SPACE : TREE_VERT;
            size_t suffix_len = strlen(suffix);
            char *child_prefix = arena_malloc(prefix_len + suffix_len + 1);
            if (!child_prefix) {
                continue;
            }
            memcpy(child_prefix, prefix, prefix_len);
            memcpy(child_prefix + prefix_len, suffix, suffix_len + 1);
            
            /* Recurse */
            collect_imports_recursive(mod_abs_path, src_dir, external_map, visited,
                                      visited_count, visited_capacity, child_prefix);
        }
    }
    
    /* Print external imports (no recursion, just display) */
    for (int i = 0; i < external_imports_count; i++) {
        const char *module_name = external_import_names[i];
        
        int is_last = (current_index == total_imports - 1);
        current_index++;
        
        /* Print current prefix + branch character */
        user_message("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);
        const char *owner = external_map ? external_module_owner_map_find(external_map, module_name) : NULL;
        if (owner) {
            user_message("%s (📦 %s)\n", module_name, owner);
        } else {
            user_message("%s (📦 external)\n", module_name);
        }
    }
}

/**
 * Convert module name (e.g., "Html.Events") to file path
 */
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

/**
 * Recursively collect all .elm files in a directory
 */
static void collect_all_elm_files(const char *dir_path, char ***files, int *count, int *capacity) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Build full path */
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            collect_all_elm_files(full_path, files, count, capacity);
        } else if (S_ISREG(st.st_mode)) {
            /* Check if .elm file */
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".elm") == 0) {
                char abs_path[MAX_PATH_LENGTH];
                if (realpath(full_path, abs_path)) {
                    DYNARRAY_PUSH(*files, *count, *capacity, arena_strdup(abs_path), char*);
                }
            }
        }
    }

    closedir(dir);
}

/**
 * Check if a file path is in a list
 */
static int is_file_in_list(const char *file, char **list, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(file, list[i]) == 0) {
            return 1;
        }
    }
    return 0;
}
