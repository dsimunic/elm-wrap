#include "debug.h"
#include "../../alloc.h"
#include "../../progname.h"
#include "../../log.h"
#include "../../dyn_array.h"
#include "../../cJSON.h"
#include "../../ast/skeleton.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

/* Forward declarations */
static int print_file_include_tree(const char *file_path);
static int print_package_include_tree(const char *dir_path);
static void collect_imports_recursive(const char *file_path, const char *src_dir,
                                       char ***visited, int *visited_count, int *visited_capacity,
                                       const char *prefix);
static char *module_name_to_path(const char *module_name, const char *src_dir);
static void collect_all_elm_files(const char *dir_path, char ***files, int *count, int *capacity);
static int is_file_in_list(const char *file, char **list, int count);
static int file_exists(const char *path);
static char *find_src_dir_for_file(const char *file_path);
static char *strip_trailing_slash(const char *path);
static char *read_file_content(const char *filepath);

/* Tree drawing characters (UTF-8) */
#define TREE_BRANCH "├── "
#define TREE_LAST   "└── "
#define TREE_VERT   "│   "
#define TREE_SPACE  "    "

static void print_include_tree_usage(void) {
    printf("Usage: %s debug include-tree <file-path>|<directory-path>\n", program_name);
    printf("\n");
    printf("Show import dependency tree for an Elm file or package.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <file-path>       Path to an Elm file (.elm)\n");
    printf("  <directory-path>  Path to a package directory (must contain elm.json)\n");
    printf("\n");
    printf("For packages:\n");
    printf("  - Shows import tree for each exposed module\n");
    printf("  - Lists redundant files not imported by any exposed module\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message\n");
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
 * Check if file exists
 */
static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * Read entire file content into allocated buffer
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
 * Strip trailing slashes from a path
 * Returns a newly allocated string
 */
static char *strip_trailing_slash(const char *path) {
    if (!path) return NULL;
    
    int len = strlen(path);
    
    /* Remove trailing slashes */
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    
    char *result = arena_malloc(len + 1);
    strncpy(result, path, len);
    result[len] = '\0';
    
    return result;
}

/**
 * Find the src directory for a given Elm file by looking for elm.json
 */
static char *find_src_dir_for_file(const char *file_path) {
    /* Get absolute path */
    char *abs_path = realpath(file_path, NULL);
    if (!abs_path) return NULL;

    char *dir = arena_strdup(abs_path);
    free(abs_path);

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
            char *src_dir = arena_malloc(strlen(dir) + 5);
            sprintf(src_dir, "%s/src", dir);
            return src_dir;
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
    char *abs_path = realpath(file_path, NULL);
    if (!abs_path) {
        log_error("Could not resolve path: %s", file_path);
        return 1;
    }

    /* Find src directory */
    char *src_dir = find_src_dir_for_file(abs_path);
    if (!src_dir) {
        printf("\n⚠️  Could not find elm.json in parent directories\n");
        printf("   Imports from external packages will not be resolved.\n");
        /* Use file's directory as fallback */
        src_dir = arena_strdup(abs_path);
        char *last_slash = strrchr(src_dir, '/');
        if (last_slash) *last_slash = '\0';
    }

    printf("\n📄 Import tree for: %s\n", abs_path);
    printf("   Source directory: %s\n\n", src_dir);

    /* Track visited files to avoid cycles */
    int visited_capacity = 64;
    int visited_count = 0;
    char **visited = arena_malloc(visited_capacity * sizeof(char*));

    /* Print the root file */
    printf("%s\n", abs_path);

    /* Recursively print imports */
    collect_imports_recursive(abs_path, src_dir, &visited, &visited_count, &visited_capacity, "");

    printf("\n");
    free(abs_path);
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
    //REVIEW: magic number.
    char elm_json_path[2048];
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
    //REVIEW: magic number.
    char src_dir[2048];
    if (source_dir_count > 0) {
        snprintf(src_dir, sizeof(src_dir), "%s/%s", clean_dir_path, source_dirs[0]);
    } else {
        snprintf(src_dir, sizeof(src_dir), "%s/src", clean_dir_path);
    }

    printf("\n📦 Import tree for package: %s\n", clean_dir_path);
    printf("   Source directory: %s\n\n", src_dir);

    /* Track all included files for redundancy detection */
    int included_capacity = 256;
    int included_count = 0;
    char **included_files = arena_malloc(included_capacity * sizeof(char*));

    /* Process exposed modules */
    if (exposed_count > 0) {
        printf("📚 Exposed Modules (%d):\n\n", exposed_count);

        for (int i = 0; i < exposed_count; i++) {
            const char *module_name = exposed_modules[i];
            char *module_path = module_name_to_path(module_name, src_dir);

            if (module_path && file_exists(module_path)) {
                /* Track visited for this tree */
                int visited_capacity = 64;
                int visited_count = 0;
                char **visited = arena_malloc(visited_capacity * sizeof(char*));

                /* Get absolute path */
                char *abs_path = realpath(module_path, NULL);
                if (abs_path) {
                    printf("%s (%s)\n", module_name, abs_path);

                    /* Add to included files */
                    DYNARRAY_PUSH(included_files, included_count, included_capacity,
                                 arena_strdup(abs_path), char*);

                    /* Print tree and collect all imports */
                    collect_imports_recursive(abs_path, src_dir, &visited, &visited_count, 
                                            &visited_capacity, "");

                    /* Add all visited files to included list */
                    for (int j = 0; j < visited_count; j++) {
                        if (!is_file_in_list(visited[j], included_files, included_count)) {
                            DYNARRAY_PUSH(included_files, included_count, included_capacity,
                                         arena_strdup(visited[j]), char*);
                        }
                    }

                    free(abs_path);
                    printf("\n");
                }
            } else {
                printf("%s (❌ NOT FOUND: %s)\n\n", module_name, 
                       module_path ? module_path : "unknown");
            }
        }
    } else {
        printf("⚠️  No exposed modules found in elm.json\n\n");
    }

    /* Collect all .elm files in src directory */
    int all_files_capacity = 256;
    int all_files_count = 0;
    char **all_files = arena_malloc(all_files_capacity * sizeof(char*));

    collect_all_elm_files(src_dir, &all_files, &all_files_count, &all_files_capacity);

    /* Find redundant files (not included by any exposed module) */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("🔍 Scanning for redundant files...\n\n");

    int redundant_count = 0;
    for (int i = 0; i < all_files_count; i++) {
        if (!is_file_in_list(all_files[i], included_files, included_count)) {
            if (redundant_count == 0) {
                printf("⚠️  Redundant files (not imported by any exposed module):\n\n");
            }
            printf("   • %s\n", all_files[i]);
            redundant_count++;
        }
    }

    if (redundant_count == 0) {
        printf("✅ No redundant files found. All files are included.\n");
    } else {
        printf("\n   Total: %d redundant file(s)\n", redundant_count);
    }

    printf("\n");
    return 0;
}

/**
 * Recursively collect and print imports for a file
 * prefix: the string to print before the branch character (accumulates "│   " or "    ")
 */
static void collect_imports_recursive(const char *file_path, const char *src_dir,
                                       char ***visited, int *visited_count, int *visited_capacity,
                                       const char *prefix) {
    /* Get absolute path for consistent comparison */
    char *abs_path = realpath(file_path, NULL);
    if (!abs_path) return;

    /* Check if already visited (cycle detection) */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp((*visited)[i], abs_path) == 0) {
            free(abs_path);
            return; /* Already processed */
        }
    }

    /* Add to visited */
    char *current_file_abs = arena_strdup(abs_path);
    DYNARRAY_PUSH(*visited, *visited_count, *visited_capacity, current_file_abs, char*);
    free(abs_path);

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
            char *mod_abs_path = realpath(module_path, NULL);
            if (mod_abs_path) {
                /* Skip self-references (shouldn't happen but be safe) */
                if (strcmp(mod_abs_path, current_file_abs) == 0) {
                    free(mod_abs_path);
                    continue;
                }
                
                /* Add to both parallel arrays - each with its own capacity */
                DYNARRAY_ENSURE_CAPACITY(local_import_names, local_imports_count, local_names_capacity, char*);
                DYNARRAY_ENSURE_CAPACITY(local_import_paths, local_imports_count, local_paths_capacity, char*);
                local_import_names[local_imports_count] = arena_strdup(module_name);
                local_import_paths[local_imports_count] = arena_strdup(mod_abs_path);
                local_imports_count++;
                free(mod_abs_path);
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
        printf("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);

        if (already_visited) {
            printf("%s (↩ already shown)\n", module_name);
        } else {
            printf("%s\n", module_name);
            
            /* Build new prefix for children */
            int prefix_len = strlen(prefix);
            char *child_prefix = arena_malloc(prefix_len + 7); /* +6 for "│   " or "    ", +1 for null */
            strcpy(child_prefix, prefix);
            strcat(child_prefix, is_last ? TREE_SPACE : TREE_VERT);
            
            /* Recurse */
            collect_imports_recursive(mod_abs_path, src_dir, visited, 
                                    visited_count, visited_capacity, child_prefix);
        }
    }
    
    /* Print external imports (no recursion, just display) */
    for (int i = 0; i < external_imports_count; i++) {
        const char *module_name = external_import_names[i];
        
        int is_last = (current_index == total_imports - 1);
        current_index++;
        
        /* Print current prefix + branch character */
        printf("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);
        printf("%s (📦 external)\n", module_name);
    }
}

/**
 * Convert module name (e.g., "Html.Events") to file path
 */
static char *module_name_to_path(const char *module_name, const char *src_dir) {
    int len = strlen(module_name);
    int src_len = strlen(src_dir);

    /* Allocate enough space: src_dir + "/" + module_name (with / instead of .) + ".elm" + null */
    char *path = arena_malloc(src_len + 1 + len + 5);

    strcpy(path, src_dir);
    strcat(path, "/");

    /* Convert dots to slashes */
    int offset = src_len + 1;
    for (int i = 0; i < len; i++) {
        if (module_name[i] == '.') {
            path[offset++] = '/';
        } else {
            path[offset++] = module_name[i];
        }
    }
    path[offset] = '\0';

    strcat(path, ".elm");

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
                /* Get absolute path */
                char *abs_path = realpath(full_path, NULL);
                if (abs_path) {
                    char *dup = arena_strdup(abs_path);
                    DYNARRAY_PUSH(*files, *count, *capacity, dup, char*);
                    free(abs_path); /* realpath uses malloc */
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
