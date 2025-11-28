/**
 * import_tree.c - Import dependency tree analysis for Elm packages
 *
 * Builds import dependency trees using the skeleton parser (tree-sitter based)
 * and detects redundant files not reachable from exposed modules.
 */

#include "import_tree.h"
#include "alloc.h"
#include "dyn_array.h"
#include "cJSON.h"
#include "ast/skeleton.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* Tree drawing characters (UTF-8) */
#define TREE_BRANCH "├── "
#define TREE_LAST   "└── "
#define TREE_VERT   "│   "
#define TREE_SPACE  "    "

/* Forward declarations */
static char *read_file_content(const char *filepath);
static int file_exists(const char *path);
static char *strip_trailing_slash(const char *path);
static char *module_name_to_path(const char *module_name, const char *src_dir);
static void collect_all_elm_files(const char *dir_path, char ***files, int *count, int *capacity);
static int is_file_in_list(const char *file, char **list, int count);
static void collect_reachable_files(const char *file_path, const char *src_dir,
                                     char ***visited, int *visited_count, int *visited_capacity);
static void print_tree_recursive(const char *file_path, const char *src_dir,
                                  char ***visited, int *visited_count, int *visited_capacity,
                                  const char *prefix, bool show_external);

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
 */
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

/**
 * Convert module name (e.g., "Html.Events") to file path
 */
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
 * Check if a file is in a list (by comparing absolute paths)
 */
static int is_file_in_list(const char *file, char **list, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(file, list[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Recursively collect all .elm files in a directory
 */
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

/**
 * Recursively collect all files reachable from a given file via imports
 * (without printing - just building the set of reachable files)
 */
static void collect_reachable_files(const char *file_path, const char *src_dir,
                                     char ***visited, int *visited_count, int *visited_capacity) {
    char *abs_path = realpath(file_path, NULL);
    if (!abs_path) return;

    /* Check if already visited */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp((*visited)[i], abs_path) == 0) {
            free(abs_path);
            return;
        }
    }

    /* Add to visited */
    char *path_copy = arena_strdup(abs_path);
    DYNARRAY_PUSH(*visited, *visited_count, *visited_capacity, path_copy, char*);
    free(abs_path);

    /* Parse the Elm file */
    SkeletonModule *mod = skeleton_parse(file_path);
    if (!mod) return;

    /* Follow local imports */
    for (int i = 0; i < mod->imports_count; i++) {
        const char *module_name = mod->imports[i].module_name;
        if (!module_name) continue;
        
        char *module_path = module_name_to_path(module_name, src_dir);
        if (module_path && file_exists(module_path)) {
            collect_reachable_files(module_path, src_dir, visited, visited_count, visited_capacity);
        }
    }
    
    skeleton_free(mod);
}

/**
 * Recursively print import tree with formatting
 */
static void print_tree_recursive(const char *file_path, const char *src_dir,
                                  char ***visited, int *visited_count, int *visited_capacity,
                                  const char *prefix, bool show_external) {
    char *abs_path = realpath(file_path, NULL);
    if (!abs_path) return;

    /* Check if already visited */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp((*visited)[i], abs_path) == 0) {
            free(abs_path);
            return;
        }
    }

    /* Add to visited */
    char *current_file_abs = arena_strdup(abs_path);
    DYNARRAY_PUSH(*visited, *visited_count, *visited_capacity, current_file_abs, char*);
    free(abs_path);

    /* Parse the Elm file */
    SkeletonModule *mod = skeleton_parse(file_path);
    if (!mod) return;

    /* Separate imports into local and external */
    int local_imports_capacity = 16;
    int local_imports_count = 0;
    char **local_import_names = arena_malloc(local_imports_capacity * sizeof(char*));
    char **local_import_paths = arena_malloc(local_imports_capacity * sizeof(char*));
    
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
                if (strcmp(mod_abs_path, current_file_abs) == 0) {
                    free(mod_abs_path);
                    continue;
                }
                
                DYNARRAY_PUSH(local_import_names, local_imports_count, local_imports_capacity, 
                             arena_strdup(module_name), char*);
                local_imports_count--;
                DYNARRAY_PUSH(local_import_paths, local_imports_count, local_imports_capacity,
                             arena_strdup(mod_abs_path), char*);
                free(mod_abs_path);
            }
        } else if (show_external) {
            DYNARRAY_PUSH(external_import_names, external_imports_count, external_imports_capacity,
                         arena_strdup(module_name), char*);
        }
    }
    
    skeleton_free(mod);

    int total_imports = local_imports_count + (show_external ? external_imports_count : 0);
    int current_index = 0;

    /* Print local imports */
    for (int i = 0; i < local_imports_count; i++) {
        const char *module_name = local_import_names[i];
        const char *mod_abs_path = local_import_paths[i];
        
        int already_visited = 0;
        for (int j = 0; j < *visited_count; j++) {
            if (strcmp((*visited)[j], mod_abs_path) == 0) {
                already_visited = 1;
                break;
            }
        }

        int is_last = (current_index == total_imports - 1);
        current_index++;
        
        printf("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);

        if (already_visited) {
            printf("%s (↩ already shown)\n", module_name);
        } else {
            printf("%s\n", module_name);
            
            int prefix_len = strlen(prefix);
            char *child_prefix = arena_malloc(prefix_len + 5);
            strcpy(child_prefix, prefix);
            strcat(child_prefix, is_last ? TREE_SPACE : TREE_VERT);
            
            print_tree_recursive(mod_abs_path, src_dir, visited, 
                                visited_count, visited_capacity, child_prefix, show_external);
        }
    }
    
    /* Print external imports */
    if (show_external) {
        for (int i = 0; i < external_imports_count; i++) {
            int is_last = (current_index == total_imports - 1);
            current_index++;
            
            printf("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);
            printf("%s (📦 external)\n", external_import_names[i]);
        }
    }
}

/**
 * Analyze a package directory and build import tree
 */
ImportTreeAnalysis *import_tree_analyze(const char *package_dir) {
    char *clean_dir = strip_trailing_slash(package_dir);
    
    /* Check for elm.json */
    char elm_json_path[2048];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", clean_dir);

    if (!file_exists(elm_json_path)) {
        arena_free(clean_dir);
        return NULL;
    }

    /* Parse exposed modules */
    int exposed_count = 0;
    char **exposed_modules = parse_exposed_modules(elm_json_path, &exposed_count);
    if (!exposed_modules) {
        arena_free(clean_dir);
        return NULL;
    }

    /* Parse source directories */
    int source_dir_count = 0;
    char **source_dirs = parse_source_directories(elm_json_path, &source_dir_count);
    
    /* Default to src if no source directories specified */
    char src_dir[2048];
    if (source_dir_count > 0) {
        snprintf(src_dir, sizeof(src_dir), "%s/%s", clean_dir, source_dirs[0]);
    } else {
        snprintf(src_dir, sizeof(src_dir), "%s/src", clean_dir);
    }

    /* Allocate analysis result */
    ImportTreeAnalysis *analysis = arena_calloc(1, sizeof(ImportTreeAnalysis));
    analysis->package_dir = clean_dir;
    analysis->src_dir = arena_strdup(src_dir);
    analysis->exposed_modules = exposed_modules;
    analysis->exposed_count = exposed_count;
    
    analysis->included_capacity = 256;
    analysis->included_count = 0;
    analysis->included_files = arena_malloc(analysis->included_capacity * sizeof(char*));
    
    analysis->redundant_capacity = 64;
    analysis->redundant_count = 0;
    analysis->redundant_files = arena_malloc(analysis->redundant_capacity * sizeof(char*));

    /* Collect all files reachable from exposed modules */
    for (int i = 0; i < exposed_count; i++) {
        const char *module_name = exposed_modules[i];
        char *module_path = module_name_to_path(module_name, src_dir);

        if (module_path && file_exists(module_path)) {
            int visited_capacity = 64;
            int visited_count = 0;
            char **visited = arena_malloc(visited_capacity * sizeof(char*));

            collect_reachable_files(module_path, src_dir, &visited, &visited_count, &visited_capacity);

            /* Add visited files to included list */
            for (int j = 0; j < visited_count; j++) {
                if (!is_file_in_list(visited[j], analysis->included_files, analysis->included_count)) {
                    DYNARRAY_PUSH(analysis->included_files, analysis->included_count, 
                                 analysis->included_capacity, arena_strdup(visited[j]), char*);
                }
            }
        }
    }

    /* Collect all .elm files in src directory */
    int all_files_capacity = 256;
    int all_files_count = 0;
    char **all_files = arena_malloc(all_files_capacity * sizeof(char*));
    collect_all_elm_files(src_dir, &all_files, &all_files_count, &all_files_capacity);
    analysis->total_files = all_files_count;

    /* Find redundant files */
    for (int i = 0; i < all_files_count; i++) {
        if (!is_file_in_list(all_files[i], analysis->included_files, analysis->included_count)) {
            DYNARRAY_PUSH(analysis->redundant_files, analysis->redundant_count,
                         analysis->redundant_capacity, arena_strdup(all_files[i]), char*);
        }
    }

    return analysis;
}

/**
 * Free resources used by an ImportTreeAnalysis
 */
void import_tree_free(ImportTreeAnalysis *analysis) {
    if (!analysis) return;
    /* Arena allocator handles freeing - nothing to do here */
}

/**
 * Check if a file path is in the included files list
 */
bool import_tree_is_included(ImportTreeAnalysis *analysis, const char *file_path) {
    if (!analysis) return false;
    return is_file_in_list(file_path, analysis->included_files, analysis->included_count);
}

/**
 * Print import tree to stdout
 */
void import_tree_print(ImportTreeAnalysis *analysis, bool show_external) {
    if (!analysis) return;

    printf("\n📦 Import tree for package: %s\n", analysis->package_dir);
    printf("   Source directory: %s\n\n", analysis->src_dir);

    if (analysis->exposed_count > 0) {
        printf("📚 Exposed Modules (%d):\n\n", analysis->exposed_count);

        for (int i = 0; i < analysis->exposed_count; i++) {
            const char *module_name = analysis->exposed_modules[i];
            char *module_path = module_name_to_path(module_name, analysis->src_dir);

            if (module_path && file_exists(module_path)) {
                int visited_capacity = 64;
                int visited_count = 0;
                char **visited = arena_malloc(visited_capacity * sizeof(char*));

                char *abs_path = realpath(module_path, NULL);
                if (abs_path) {
                    printf("%s (%s)\n", module_name, abs_path);
                    
                    print_tree_recursive(abs_path, analysis->src_dir, &visited, 
                                        &visited_count, &visited_capacity, "", show_external);
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
}

/**
 * Print just the redundant files summary
 */
void import_tree_print_redundant(ImportTreeAnalysis *analysis) {
    if (!analysis) return;

    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("🔍 Scanning for redundant files...\n\n");

    if (analysis->redundant_count == 0) {
        printf("✅ No redundant files found. All files are included.\n");
    } else {
        printf("⚠️  Redundant files (not imported by any exposed module):\n\n");
        for (int i = 0; i < analysis->redundant_count; i++) {
            printf("   • %s\n", analysis->redundant_files[i]);
        }
        printf("\n   Total: %d redundant file(s)\n", analysis->redundant_count);
    }
    printf("\n");
}

/**
 * Get count of redundant files
 */
int import_tree_redundant_count(ImportTreeAnalysis *analysis) {
    if (!analysis) return 0;
    return analysis->redundant_count;
}
