/**
 * builder.c - Pre-build artifact cleanup for local development
 *
 * Cleans Elm compiler artifacts (artifacts.dat, artifacts.x.dat) for local-dev
 * packages before compilation, ensuring that code changes are always picked up.
 */

#include "builder.h"
#include "../package/install_local_dev.h"
#include "../../local_dev/local_dev_tracking.h"
#include "../../elm_json.h"
#include "../../cache.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../fileutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/**
 * Delete a file if it exists.
 * Returns true on success or if file doesn't exist.
 */
static bool delete_file_if_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        /* File doesn't exist, that's fine */
        return true;
    }

    if (!S_ISREG(st.st_mode)) {
        log_debug("Not a regular file, skipping: %s", path);
        return true;
    }

    if (unlink(path) != 0) {
        log_debug("Failed to delete: %s", path);
        return false;
    }

    log_debug("Deleted artifact: %s", path);
    return true;
}

/**
 * Delete artifacts.dat and artifacts.x.dat from a directory.
 */
static bool delete_artifacts_in_dir(const char *dir_path) {
    if (!dir_path) return false;

    bool success = true;

    /* Delete artifacts.dat */
    size_t artifacts_len = strlen(dir_path) + strlen("/artifacts.dat") + 1;
    char *artifacts_path = arena_malloc(artifacts_len);
    if (artifacts_path) {
        snprintf(artifacts_path, artifacts_len, "%s/artifacts.dat", dir_path);
        if (!delete_file_if_exists(artifacts_path)) {
            success = false;
        }
        arena_free(artifacts_path);
    }

    /* Delete artifacts.x.dat (extended artifacts) */
    size_t artifacts_x_len = strlen(dir_path) + strlen("/artifacts.x.dat") + 1;
    char *artifacts_x_path = arena_malloc(artifacts_x_len);
    if (artifacts_x_path) {
        snprintf(artifacts_x_path, artifacts_x_len, "%s/artifacts.x.dat", dir_path);
        if (!delete_file_if_exists(artifacts_x_path)) {
            success = false;
        }
        arena_free(artifacts_x_path);
    }

    return success;
}

/**
 * Delete elm-stuff directory from a directory.
 */
static bool delete_elm_stuff_in_dir(const char *dir_path) {
    if (!dir_path) return false;

    size_t elm_stuff_len = strlen(dir_path) + strlen("/elm-stuff") + 1;
    char *elm_stuff_path = arena_malloc(elm_stuff_len);
    if (!elm_stuff_path) return false;

    snprintf(elm_stuff_path, elm_stuff_len, "%s/elm-stuff", dir_path);

    bool success = true;
    struct stat st;
    if (stat(elm_stuff_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        log_debug("Deleting elm-stuff directory: %s", elm_stuff_path);
        if (!remove_directory_recursive(elm_stuff_path)) {
            log_debug("Failed to delete elm-stuff: %s", elm_stuff_path);
            success = false;
        }
    }

    arena_free(elm_stuff_path);
    return success;
}

/**
 * Get the directory containing elm.json.
 * Returns arena-allocated absolute path string, or NULL on error.
 */
static char *get_elm_json_dir(const char *elm_json_path) {
    char abs_path[PATH_MAX];

    if (realpath(elm_json_path, abs_path) == NULL) {
        return NULL;
    }

    /* Find the last slash to get the directory */
    char *last_slash = strrchr(abs_path, '/');
    if (!last_slash) {
        return arena_strdup(".");
    }

    /* Handle root directory case */
    if (last_slash == abs_path) {
        return arena_strdup("/");
    }

    *last_slash = '\0';
    return arena_strdup(abs_path);
}

/**
 * Clean build artifacts for local-dev packages.
 */
bool builder_clean_local_dev_artifacts(const char *elm_json_path, CacheConfig *cache) {
    if (!elm_json_path) {
        return false;
    }

    /* Read elm.json to determine project type */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_debug("builder: Could not read elm.json at %s", elm_json_path);
        return true;  /* Not an error - maybe elm.json doesn't exist yet */
    }

    bool success = true;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        /* For applications, find and clean all tracked local-dev packages */
        int pkg_count = 0;
        LocalDevPackage *packages = local_dev_get_tracked_packages(elm_json_path, &pkg_count);

        if (pkg_count > 0 && packages && cache) {
            log_debug("builder: Found %d tracked local-dev package(s)", pkg_count);

            /* Delete elm-stuff in the application directory */
            char *app_dir = get_elm_json_dir(elm_json_path);
            if (app_dir) {
                log_debug("builder: Cleaning elm-stuff for application at %s", app_dir);
                if (!delete_elm_stuff_in_dir(app_dir)) {
                    success = false;
                }
                arena_free(app_dir);
            }

            for (int i = 0; i < pkg_count; i++) {
                char *pkg_path = cache_get_package_path(cache,
                    packages[i].author, packages[i].name, packages[i].version);

                if (pkg_path) {
                    log_debug("builder: Cleaning artifacts for %s/%s %s at %s",
                        packages[i].author, packages[i].name, packages[i].version, pkg_path);

                    if (!delete_artifacts_in_dir(pkg_path)) {
                        success = false;
                    }
                    arena_free(pkg_path);
                }
            }
            local_dev_packages_free(packages, pkg_count);
        }
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        /* For packages, clean artifacts in the project root directory */
        char *project_dir = get_elm_json_dir(elm_json_path);
        if (project_dir) {
            log_debug("builder: Cleaning artifacts for package at %s", project_dir);
            if (!delete_artifacts_in_dir(project_dir)) {
                success = false;
            }
            arena_free(project_dir);
        }
    }

    elm_json_free(elm_json);
    return success;
}
