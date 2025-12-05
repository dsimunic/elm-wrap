/**
 * elm_project.c - Elm project file utilities
 */

#include "elm_project.h"
#include "fileutil.h"
#include "alloc.h"
#include "vendor/cJSON.h"
#include "dyn_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

char **elm_parse_exposed_modules(const char *elm_json_path, int *count) {
    char *content = file_read_contents(elm_json_path);
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

char **elm_parse_source_directories(const char *elm_json_path, int *count) {
    char *content = file_read_contents(elm_json_path);
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

char *elm_module_name_to_path(const char *module_name, const char *src_dir) {
    int len = strlen(module_name);
    int src_len = strlen(src_dir);

    /* Allocate enough space: src_dir + "/" + module_name (with / instead of .) + ".elm" + null */
    char *path = arena_malloc(src_len + 1 + len + 5);
    strcpy(path, src_dir);
    strcat(path, "/");

    /* Convert dots to slashes */
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

void elm_collect_elm_files(const char *dir_path, char ***files, int *count, int *capacity) {
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
                elm_collect_elm_files(full_path, files, count, capacity);
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

void elm_collect_all_files(const char *dir_path, char ***files, int *count, int *capacity) {
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
                elm_collect_all_files(full_path, files, count, capacity);
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

int elm_is_file_in_list(const char *file, char **list, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(file, list[i]) == 0) {
            return 1;
        }
    }
    return 0;
}
