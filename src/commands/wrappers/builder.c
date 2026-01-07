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
#include "../../shared/log.h"
#include "../../fileutil.h"
#include "elm_cmd_common.h"
#include "../../elm_project.h"
#include "../../elm_compiler.h"
#include "../../global_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>

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
        log_debug("builder: Not a regular file, skipping: %s", path);
        return true;
    }

    if (unlink(path) != 0) {
        log_debug("builder: Failed to delete: %s", path);
        return false;
    }

    log_debug("builder: Deleted artifact: %s", path);
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
        log_debug("builder: Deleting elm-stuff directory: %s", elm_stuff_path);
        if (!remove_directory_recursive(elm_stuff_path)) {
            log_debug("builder: Failed to delete elm-stuff: %s", elm_stuff_path);
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
 * Re-run compiler without --report=json to get human-readable error output.
 * Returns arena-allocated string with compiler output from the first failing module.
 */
static char *run_compiler_for_human_errors(
    const char *package_dir_abs,
    char **exposed_modules,
    int exposed_count
) {
    if (!package_dir_abs || !exposed_modules || exposed_count <= 0) {
        return arena_strdup("Internal error: invalid parameters for error re-run");
    }

    char *compiler_path = elm_compiler_get_path();
    if (!compiler_path) {
        return arena_strdup("Internal error: could not find compiler");
    }

    char **compiler_env = build_elm_environment();
    if (!compiler_env) {
        return arena_strdup("Internal error: could not build environment");
    }

    const char *compiler_name = global_context_compiler_name();

    /* Try to compile each exposed module until we find the error */
    for (int i = 0; i < exposed_count; i++) {
        const char *module_name = exposed_modules[i];
        if (!module_name) continue;

        char *rel_elm = elm_module_name_to_path(module_name, "src");
        if (!rel_elm) continue;

        char abs_elm[PATH_MAX];
        snprintf(abs_elm, sizeof(abs_elm), "%s/%s", package_dir_abs, rel_elm);

        /* Skip if file doesn't exist */
        if (!file_exists(abs_elm)) {
            continue;
        }

        /* Build args WITHOUT --report=json */
        char **elm_args = arena_malloc(sizeof(char*) * 6);
        elm_args[0] = (char*)compiler_name;
        elm_args[1] = "make";
        elm_args[2] = rel_elm;
        elm_args[3] = "--output";
        elm_args[4] = "/dev/null";
        elm_args[5] = NULL;

        char *compiler_stdout = NULL;
        int exit_code = run_compiler_make_capture_stdout_in_dir(
            compiler_path,
            compiler_env,
            elm_args,
            package_dir_abs,
            &compiler_stdout
        );

        arena_free(elm_args);

        /* If we found an error, return the output */
        if (exit_code != 0 && compiler_stdout) {
            return compiler_stdout;
        }
    }

    /* Shouldn't happen (silent build reported error but we can't reproduce it) */
    return arena_strdup("Compilation failed but could not capture error output");
}

/**
 * Compile all tracked local-dev packages before main compilation.
 */
bool builder_compile_local_dev_packages(
    const char *elm_json_path,
    CacheConfig *cache,
    char **out_error_output
) {
    if (out_error_output) *out_error_output = NULL;
    if (!elm_json_path || !cache) return true;

    /* Read elm.json to determine project type */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_debug("builder: Could not read elm.json at %s", elm_json_path);
        return true;  /* Not an error - maybe elm.json doesn't exist yet */
    }

    /* Only process for applications (packages don't have local-dev dependencies) */
    if (elm_json->type != ELM_PROJECT_APPLICATION) {
        elm_json_free(elm_json);
        return true;
    }

    elm_json_free(elm_json);

    /* Get tracked local-dev packages */
    int pkg_count = 0;
    LocalDevPackage *packages = local_dev_get_tracked_packages(elm_json_path, &pkg_count);

    if (pkg_count == 0 || !packages) {
        /* No local-dev packages to compile */
        if (packages) {
            local_dev_packages_free(packages, pkg_count);
        }
        return true;
    }

    log_debug("builder: Compiling %d local-dev package(s)...", pkg_count);

    bool success = true;

    /* Compile each tracked package */
    for (int i = 0; i < pkg_count; i++) {
        const char *author = packages[i].author;
        const char *name = packages[i].name;
        const char *version = packages[i].version;

        /* Get package path */
        char *pkg_path = cache_get_package_path(cache, author, name, version);
        if (!pkg_path) {
            log_debug("builder: Could not get path for %s/%s %s", author, name, version);
            continue;
        }

        /* Build elm.json path */
        size_t elm_json_len = strlen(pkg_path) + strlen("/elm.json") + 1;
        char *pkg_elm_json_path = arena_malloc(elm_json_len);
        snprintf(pkg_elm_json_path, elm_json_len, "%s/elm.json", pkg_path);

        /* Parse exposed modules */
        int exposed_count = 0;
        char **exposed_modules = elm_parse_exposed_modules(pkg_elm_json_path, &exposed_count);

        if (!exposed_modules || exposed_count == 0) {
            log_debug("builder: Package %s/%s %s has no exposed modules, skipping",
                author, name, version);
            arena_free(pkg_elm_json_path);
            arena_free(pkg_path);
            continue;
        }

        log_debug("builder: Compiling local-dev package: %s/%s %s", author, name, version);
        log_debug("builder: Package path: %s", pkg_path);
        log_debug("builder: Exposed modules: %d", exposed_count);

        /* Run silent build with --report=json */
        /* Pass false for clean_artifacts - we don't want to delete artifacts yet */
        char *json_output = NULL;
        bool compile_ok = elm_cmd_run_silent_package_build(
            pkg_path,
            pkg_elm_json_path,
            exposed_modules,
            exposed_count,
            false,  /* clean_artifacts */
            &json_output
        );

        if (!compile_ok) {
            /* Compilation failed - re-run without --report=json for human-friendly errors */
            log_error("Local-dev package %s/%s %s failed to compile", author, name, version);

            char *error_output = run_compiler_for_human_errors(
                pkg_path,
                exposed_modules,
                exposed_count
            );

            if (out_error_output) {
                *out_error_output = error_output;
            }

            arena_free(pkg_elm_json_path);
            arena_free(pkg_path);
            success = false;
            break;
        }

        arena_free(pkg_elm_json_path);
        arena_free(pkg_path);
    }

    local_dev_packages_free(packages, pkg_count);

    if (success) {
        log_debug("builder: All local-dev packages compiled successfully");
    }

    return success;
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
