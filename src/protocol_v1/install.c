/**
 * protocol_v1/install.c - V1 Protocol Install Functions Implementation
 *
 * Functions for package dependency display using the V1 protocol.
 * These functions require network access and package downloads to read
 * package elm.json files from the cache.
 */

#include "install.h"
#include "../elm_json.h"
#include "../cache.h"
#include "../constants.h"
#include "../log.h"
#include "../alloc.h"
#include "../fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#define ELM_JSON_PATH "elm.json"

/**
 * Helper function to find elm.json in a package directory.
 * Handles both cases: elm.json at root and elm.json in a subdirectory.
 */
static char* v1_find_package_elm_json(const char *pkg_path) {
    /* Try direct path first */
    size_t direct_len = strlen(pkg_path) + strlen("/elm.json") + 1;
    char *direct_path = arena_malloc(direct_len);
    if (!direct_path) return NULL;
    snprintf(direct_path, direct_len, "%s/elm.json", pkg_path);

    struct stat st;
    if (stat(direct_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return direct_path;
    }
    arena_free(direct_path);

    /* Not found at root - look in subdirectories */
    DIR *dir = opendir(pkg_path);
    if (!dir) return NULL;

    struct dirent *entry;
    char *found_path = NULL;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t subdir_len = strlen(pkg_path) + strlen(entry->d_name) + 2;
        char *subdir_path = arena_malloc(subdir_len);
        if (!subdir_path) continue;
        snprintf(subdir_path, subdir_len, "%s/%s", pkg_path, entry->d_name);

        if (stat(subdir_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t elm_json_len = strlen(subdir_path) + strlen("/elm.json") + 1;
            char *elm_json_path = arena_malloc(elm_json_len);
            if (elm_json_path) {
                snprintf(elm_json_path, elm_json_len, "%s/elm.json", subdir_path);
                if (stat(elm_json_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    found_path = elm_json_path;
                    arena_free(subdir_path);
                    break;
                }
                arena_free(elm_json_path);
            }
        }
        arena_free(subdir_path);
    }

    closedir(dir);
    return found_path;
}

bool v1_package_depends_on(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                           const char *target_author, const char *target_name,
                           InstallEnv *env) {
    char *pkg_path = cache_get_package_path(env->cache, pkg_author, pkg_name, pkg_version);
    if (!pkg_path) {
        return false;
    }

    char *elm_json_path = v1_find_package_elm_json(pkg_path);
    ElmJson *pkg_elm_json = NULL;

    if (elm_json_path) {
        pkg_elm_json = elm_json_read(elm_json_path);
        arena_free(elm_json_path);
    }

    if (!pkg_elm_json) {
        if (cache_download_package_with_env(env, pkg_author, pkg_name, pkg_version)) {
            elm_json_path = v1_find_package_elm_json(pkg_path);
            if (elm_json_path) {
                pkg_elm_json = elm_json_read(elm_json_path);
                arena_free(elm_json_path);
            }
        }
    }

    arena_free(pkg_path);

    if (!pkg_elm_json) {
        return false;
    }

    bool depends = false;

    if (pkg_elm_json->package_dependencies) {
        Package *dep = package_map_find(pkg_elm_json->package_dependencies, target_author, target_name);
        if (dep) {
            depends = true;
        }
    }

    if (!depends && pkg_elm_json->package_test_dependencies) {
        Package *dep = package_map_find(pkg_elm_json->package_test_dependencies, target_author, target_name);
        if (dep) {
            depends = true;
        }
    }

    elm_json_free(pkg_elm_json);
    return depends;
}

int v1_show_package_dependencies(const char *author, const char *name, const char *version,
                                 InstallEnv *env) {
    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        log_error("Failed to get package path");
        return 1;
    }

    char *elm_json_path = v1_find_package_elm_json(pkg_path);

    ElmJson *elm_json = NULL;
    if (elm_json_path) {
        elm_json = elm_json_read(elm_json_path);
    }

    if (!elm_json) {
        log_debug("Package not in cache, attempting download");
        if (!cache_download_package_with_env(env, author, name, version)) {
            log_error("Failed to download package %s/%s %s", author, name, version);
            if (elm_json_path) arena_free(elm_json_path);
            arena_free(pkg_path);
            return 1;
        }

        if (elm_json_path) arena_free(elm_json_path);
        elm_json_path = find_package_elm_json(pkg_path);

        if (elm_json_path) {
            elm_json = elm_json_read(elm_json_path);
        }

        if (!elm_json) {
            log_error("Failed to read elm.json for %s/%s %s", author, name, version);
            if (elm_json_path) arena_free(elm_json_path);
            arena_free(pkg_path);
            return 1;
        }
    }

    if (elm_json_path) arena_free(elm_json_path);
    arena_free(pkg_path);

    printf("\n");
    printf("Package: %s/%s %s\n", author, name, version);
    printf("========================================\n\n");

    if (elm_json->type == ELM_PROJECT_PACKAGE && elm_json->package_dependencies) {
        PackageMap *deps = elm_json->package_dependencies;

        if (deps->count == 0) {
            printf("No dependencies\n");
        } else {
            int max_width = 0;
            for (int i = 0; i < deps->count; i++) {
                Package *pkg = &deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            if (elm_json->package_test_dependencies && elm_json->package_test_dependencies->count > 0) {
                PackageMap *test_deps = elm_json->package_test_dependencies;
                for (int i = 0; i < test_deps->count; i++) {
                    Package *pkg = &test_deps->packages[i];
                    int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                    if (pkg_len > max_width) max_width = pkg_len;
                }
            }

            printf("Dependencies (%d):\n", deps->count);
            for (int i = 0; i < deps->count; i++) {
                Package *pkg = &deps->packages[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s    %s\n", max_width, pkg_name, pkg->version);
            }
        }

        if (elm_json->package_test_dependencies && elm_json->package_test_dependencies->count > 0) {
            PackageMap *test_deps = elm_json->package_test_dependencies;

            int max_width = 0;
            for (int i = 0; i < test_deps->count; i++) {
                Package *pkg = &test_deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            if (elm_json->package_dependencies) {
                for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                    Package *pkg = &elm_json->package_dependencies->packages[i];
                    int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                    if (pkg_len > max_width) max_width = pkg_len;
                }
            }

            printf("\nTest Dependencies (%d):\n", test_deps->count);
            for (int i = 0; i < test_deps->count; i++) {
                Package *pkg = &test_deps->packages[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s    %s\n", max_width, pkg_name, pkg->version);
            }
        }
    } else {
        printf("(Not a package - this is an application)\n");
    }

    /* Check for reverse dependencies in current elm.json */
    ElmJson *current_elm_json = elm_json_read(ELM_JSON_PATH);
    if (current_elm_json) {
        PackageMap *all_deps = package_map_create();

        if (current_elm_json->dependencies_direct) {
            for (int i = 0; i < current_elm_json->dependencies_direct->count; i++) {
                Package *pkg = &current_elm_json->dependencies_direct->packages[i];
                package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (current_elm_json->dependencies_indirect) {
            for (int i = 0; i < current_elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &current_elm_json->dependencies_indirect->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->dependencies_test_direct) {
            for (int i = 0; i < current_elm_json->dependencies_test_direct->count; i++) {
                Package *pkg = &current_elm_json->dependencies_test_direct->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->dependencies_test_indirect) {
            for (int i = 0; i < current_elm_json->dependencies_test_indirect->count; i++) {
                Package *pkg = &current_elm_json->dependencies_test_indirect->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        /* Note: We skip package_dependencies and package_test_dependencies here because
         * package elm.json files contain version ranges (e.g., "1.0.5 <= v < 2.0.0")
         * rather than concrete versions. We can only check reverse dependencies for
         * concrete versions that can be downloaded from the registry.
         */

        PackageMap *reverse_deps = package_map_create();

        for (int i = 0; i < all_deps->count; i++) {
            Package *pkg = &all_deps->packages[i];

            /* Skip the target package itself */
            if (strcmp(pkg->author, author) == 0 && strcmp(pkg->name, name) == 0) {
                continue;
            }

            if (v1_package_depends_on(pkg->author, pkg->name, pkg->version, author, name, env)) {
                package_map_add(reverse_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (reverse_deps->count > 0) {
            /* Calculate max width for aligned output */
            int max_width = 0;
            for (int i = 0; i < reverse_deps->count; i++) {
                Package *pkg = &reverse_deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            printf("\nPackages in elm.json that depend on %s/%s (%d):\n", author, name, reverse_deps->count);
            for (int i = 0; i < reverse_deps->count; i++) {
                Package *pkg = &reverse_deps->packages[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s    %s\n", max_width, pkg_name, pkg->version);
            }
        }

        package_map_free(reverse_deps);
        package_map_free(all_deps);
        elm_json_free(current_elm_json);
    }

    printf("\n");
    elm_json_free(elm_json);
    return 0;
}
