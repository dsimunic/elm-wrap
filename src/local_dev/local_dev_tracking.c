/**
 * local_dev_tracking.c - Shared local development package tracking implementation
 *
 * Provides functions for querying local-dev package tracking relationships.
 * This module consolidates tracking query logic previously duplicated in
 * info_cmd.c, builder.c, and install_local_dev.c.
 */

#include "local_dev_tracking.h"
#include "../commands/package/install_local_dev.h"
#include "../alloc.h"
#include "../constants.h"
#include "../fileutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH_LENGTH
#endif

void local_dev_package_free(LocalDevPackage *pkg) {
    if (!pkg) return;
    if (pkg->author) arena_free(pkg->author);
    if (pkg->name) arena_free(pkg->name);
    if (pkg->version) arena_free(pkg->version);
}

void local_dev_packages_free(LocalDevPackage *pkgs, int count) {
    if (!pkgs) return;
    for (int i = 0; i < count; i++) {
        local_dev_package_free(&pkgs[i]);
    }
    arena_free(pkgs);
}

LocalDevPackage *local_dev_get_tracked_packages(const char *elm_json_path, int *out_count) {
    *out_count = 0;

    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return NULL;
    }

    /* Get absolute path of the elm.json to match against tracking files */
    char abs_elm_json_path[MAX_PATH_LENGTH];
    if (realpath(elm_json_path, abs_elm_json_path) == NULL) {
        /* If realpath fails, use the original path */
        strncpy(abs_elm_json_path, elm_json_path, MAX_PATH_LENGTH - 1);
        abs_elm_json_path[MAX_PATH_LENGTH - 1] = '\0';
    }

    int capacity = INITIAL_SMALL_CAPACITY;
    int count = 0;
    LocalDevPackage *packages = arena_malloc((size_t)capacity * sizeof(LocalDevPackage));
    if (!packages) {
        arena_free(tracking_dir);
        return NULL;
    }

    /* Scan tracking directory structure: tracking_dir/author/name/version/hash */
    DIR *author_dir = opendir(tracking_dir);
    if (!author_dir) {
        arena_free(tracking_dir);
        arena_free(packages);
        return NULL;
    }

    struct dirent *author_entry;
    while ((author_entry = readdir(author_dir)) != NULL) {
        if (author_entry->d_name[0] == '.') continue;

        size_t author_path_len = strlen(tracking_dir) + strlen(author_entry->d_name) + 2;
        char *author_path = arena_malloc(author_path_len);
        if (!author_path) continue;
        snprintf(author_path, author_path_len, "%s/%s", tracking_dir, author_entry->d_name);

        struct stat st;
        if (stat(author_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            arena_free(author_path);
            continue;
        }

        DIR *name_dir = opendir(author_path);
        if (!name_dir) {
            arena_free(author_path);
            continue;
        }

        struct dirent *name_entry;
        while ((name_entry = readdir(name_dir)) != NULL) {
            if (name_entry->d_name[0] == '.') continue;

            size_t name_path_len = strlen(author_path) + strlen(name_entry->d_name) + 2;
            char *name_path = arena_malloc(name_path_len);
            if (!name_path) continue;
            snprintf(name_path, name_path_len, "%s/%s", author_path, name_entry->d_name);

            if (stat(name_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                arena_free(name_path);
                continue;
            }

            DIR *version_dir_handle = opendir(name_path);
            if (!version_dir_handle) {
                arena_free(name_path);
                continue;
            }

            struct dirent *version_entry;
            while ((version_entry = readdir(version_dir_handle)) != NULL) {
                if (version_entry->d_name[0] == '.') continue;

                size_t version_path_len = strlen(name_path) + strlen(version_entry->d_name) + 2;
                char *version_path = arena_malloc(version_path_len);
                if (!version_path) continue;
                snprintf(version_path, version_path_len, "%s/%s", name_path, version_entry->d_name);

                if (stat(version_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    arena_free(version_path);
                    continue;
                }

                /* Check all tracking files in this version directory */
                DIR *tracking_files = opendir(version_path);
                if (!tracking_files) {
                    arena_free(version_path);
                    continue;
                }

                bool found_match = false;
                struct dirent *tracking_entry;
                while ((tracking_entry = readdir(tracking_files)) != NULL) {
                    if (tracking_entry->d_name[0] == '.') continue;

                    size_t tracking_file_len = strlen(version_path) + strlen(tracking_entry->d_name) + 2;
                    char *tracking_file = arena_malloc(tracking_file_len);
                    if (!tracking_file) continue;
                    snprintf(tracking_file, tracking_file_len, "%s/%s", version_path, tracking_entry->d_name);

                    char *content = file_read_contents_bounded(tracking_file, MAX_PATH_LENGTH, NULL);
                    arena_free(tracking_file);
                    if (!content) continue;

                    /* Strip trailing newline */
                    size_t content_len = strlen(content);
                    if (content_len > 0 && content[content_len - 1] == '\n') {
                        content[content_len - 1] = '\0';
                    }

                    /* Compare with our elm.json path */
                    if (strcmp(content, abs_elm_json_path) == 0) {
                        found_match = true;
                        arena_free(content);
                        break;
                    }
                    arena_free(content);
                }

                closedir(tracking_files);

                /* If we found a match, add this package to the list */
                if (found_match) {
                    if (count >= capacity) {
                        capacity *= 2;
                        packages = arena_realloc(packages, (size_t)capacity * sizeof(LocalDevPackage));
                        if (!packages) {
                            arena_free(version_path);
                            closedir(version_dir_handle);
                            arena_free(name_path);
                            closedir(name_dir);
                            arena_free(author_path);
                            closedir(author_dir);
                            arena_free(tracking_dir);
                            return NULL;
                        }
                    }

                    packages[count].author = arena_strdup(author_entry->d_name);
                    packages[count].name = arena_strdup(name_entry->d_name);
                    packages[count].version = arena_strdup(version_entry->d_name);
                    count++;
                }

                arena_free(version_path);
            }

            closedir(version_dir_handle);
            arena_free(name_path);
        }

        closedir(name_dir);
        arena_free(author_path);
    }

    closedir(author_dir);
    arena_free(tracking_dir);

    *out_count = count;
    return count > 0 ? packages : NULL;
}

char **local_dev_get_tracking_apps(const char *author, const char *name,
                                   const char *version, int *out_count) {
    *out_count = 0;

    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return NULL;
    }

    /* Build path: tracking_dir/author/name/version */
    size_t dir_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *version_dir = arena_malloc(dir_len);
    if (!version_dir) {
        arena_free(tracking_dir);
        return NULL;
    }
    snprintf(version_dir, dir_len, "%s/%s/%s/%s", tracking_dir, author, name, version);
    arena_free(tracking_dir);

    DIR *dir = opendir(version_dir);
    if (!dir) {
        arena_free(version_dir);
        return NULL;
    }

    /* Collect paths */
    int capacity = INITIAL_SMALL_CAPACITY;
    int count = 0;
    char **paths = arena_malloc((size_t)capacity * sizeof(char *));
    if (!paths) {
        closedir(dir);
        arena_free(version_dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Read the tracking file to get the app elm.json path */
        size_t file_len = strlen(version_dir) + strlen(entry->d_name) + 2;
        char *tracking_file = arena_malloc(file_len);
        if (!tracking_file) continue;
        snprintf(tracking_file, file_len, "%s/%s", version_dir, entry->d_name);

        char *content = file_read_contents_bounded(tracking_file, MAX_PATH_LENGTH, NULL);
        arena_free(tracking_file);
        if (!content) continue;

        /* Strip trailing newline */
        size_t content_len = strlen(content);
        if (content_len > 0 && content[content_len - 1] == '\n') {
            content[content_len - 1] = '\0';
        }

        /* Check if the elm.json still exists */
        struct stat st;
        if (stat(content, &st) != 0) {
            arena_free(content);
            continue;
        }

        /* Add to list */
        if (count >= capacity) {
            capacity *= 2;
            paths = arena_realloc(paths, (size_t)capacity * sizeof(char *));
            if (!paths) {
                arena_free(content);
                break;
            }
        }
        paths[count++] = content;
    }

    closedir(dir);
    arena_free(version_dir);

    *out_count = count;
    return count > 0 ? paths : NULL;
}
//REVIEW: This seems very inefficient when called from a loop. Maybe it will make sense to cache all packages in the context?
bool is_package_local_dev(const char *author, const char *name, const char *version) {
    if (!author || !name || !version) {
        return false;
    }

    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return false;
    }

    /* Build path: tracking_dir/author/name/version */
    size_t dir_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *version_dir = arena_malloc(dir_len);
    if (!version_dir) {
        arena_free(tracking_dir);
        return false;
    }
    snprintf(version_dir, dir_len, "%s/%s/%s/%s", tracking_dir, author, name, version);
    arena_free(tracking_dir);

    /* Check if the directory exists */
    struct stat st;
    bool exists = (stat(version_dir, &st) == 0 && S_ISDIR(st.st_mode));
    arena_free(version_dir);

    return exists;
}
