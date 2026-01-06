/**
 * elm_cmd_common.c - Common utilities for Elm command wrappers
 */

#include "elm_cmd_common.h"
#include "../../cache.h"
#include "../../registry.h"
#include "../../alloc.h"
#include "../../messages.h"
#include "../../shared/log.h"
#include "../../constants.h"
#include "../../commands/package/package_common.h"
#include "../../local_dev/local_dev_tracking.h"
#include "../../elm_compiler.h"
#include "../../elm_project.h"
#include "../../fileutil.h"
#include "../../global_context.h"
#include "builder.h"
#include "../../vendor/cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static int compare_string_ptrs_for_sort(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;

    if (!sa && !sb) return 0;
    if (!sa) return -1;
    if (!sb) return 1;
    return strcmp(sa, sb);
}

int elm_cmd_get_compiler_error_paths(const char *compiler_json, char ***out_paths) {
    if (out_paths) *out_paths = NULL;
    if (!compiler_json || compiler_json[0] == '\0') {
        return 0;
    }

    cJSON *root = cJSON_Parse(compiler_json);
    if (!root) {
        return 0;
    }

    cJSON *errors = cJSON_GetObjectItem(root, "errors");
    if (!errors || !cJSON_IsArray(errors)) {
        cJSON_Delete(root);
        return 0;
    }

    int paths_capacity = INITIAL_SMALL_CAPACITY;
    int paths_count = 0;
    char **paths = arena_malloc(paths_capacity * sizeof(char*));
    if (!paths) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *err = NULL;
    cJSON_ArrayForEach(err, errors) {
        if (!err || !cJSON_IsObject(err)) continue;

        cJSON *path = cJSON_GetObjectItem(err, "path");
        if (!path || !cJSON_IsString(path) || !path->valuestring || path->valuestring[0] == '\0') {
            continue;
        }

        bool seen = false;
        for (int i = 0; i < paths_count; i++) {
            if (paths[i] && strcmp(paths[i], path->valuestring) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) continue;

        if (paths_count >= paths_capacity) {
            paths_capacity *= 2;
            paths = arena_realloc(paths, paths_capacity * sizeof(char*));
            if (!paths) {
                cJSON_Delete(root);
                return 0;
            }
        }
        paths[paths_count++] = arena_strdup(path->valuestring);
    }

    cJSON_Delete(root);

    if (paths_count > 1) {
        qsort(paths, (size_t)paths_count, sizeof(char*), compare_string_ptrs_for_sort);
    }

    if (out_paths) {
        *out_paths = paths;
    }
    return paths_count;
}

int elm_cmd_count_compiler_error_files(const char *compiler_json) {
    return elm_cmd_get_compiler_error_paths(compiler_json, NULL);
}

char *elm_cmd_path_relative_to_base(const char *abs_path, const char *base_abs) {
    if (!abs_path) return NULL;
    if (!base_abs || base_abs[0] == '\0') return arena_strdup(abs_path);

    size_t base_len = strlen(base_abs);
    if (strncmp(abs_path, base_abs, base_len) == 0) {
        const char *rest = abs_path + base_len;
        while (*rest == '/') rest++;
        return arena_strdup(rest);
    }

    return arena_strdup(abs_path);
}

char **build_elm_environment(void) {
    extern char **environ;

    // Check if we should keep elm online
    const char *keep_online = getenv("WRAP_ALLOW_ELM_ONLINE");

    // Count current environment variables
    int env_count = 0;
    while (environ[env_count] != NULL) {
        env_count++;
    }

    // Allocate new environment array
    // +1 for https_proxy (if needed), +1 for NULL terminator
    int new_count = env_count + (keep_online ? 0 : 1) + 1;
    char **new_env = arena_malloc(sizeof(char*) * new_count);
    if (!new_env) {
        return NULL;
    }

    // Copy existing environment
    for (int i = 0; i < env_count; i++) {
        new_env[i] = environ[i];
    }

    // Add https_proxy to force offline mode (unless user wants to keep online)
    if (!keep_online) {
        new_env[env_count] = "https_proxy=http://1";
        new_env[env_count + 1] = NULL;
    } else {
        new_env[env_count] = NULL;
    }

    return new_env;
}

/**
 * LocalDevPackageInfo - Information about a package tracked for local development
 */
typedef struct {
    char *author;
    char *name;
    char *version;
} LocalDevPackageInfo;

/**
 * Helper function to track a package if it's a local-dev package.
 * Returns true if the package was tracked, false otherwise.
 */
static bool track_if_local_dev(
    Package *pkg,
    LocalDevPackageInfo **local_dev_packages,
    int *local_dev_count,
    int *local_dev_capacity
) {
    if (!is_package_local_dev(pkg->author, pkg->name, pkg->version)) {
        return false;
    }

    /* Expand capacity if needed */
    if (*local_dev_count >= *local_dev_capacity) {
        *local_dev_capacity *= 2;
        *local_dev_packages = arena_realloc(*local_dev_packages, (*local_dev_capacity) * sizeof(LocalDevPackageInfo));
        if (!*local_dev_packages) {
            log_error("Failed to reallocate memory for local-dev package tracking");
            return false;
        }
    }

    /* Track the package */
    (*local_dev_packages)[*local_dev_count].author = pkg->author;
    (*local_dev_packages)[*local_dev_count].name = pkg->name;
    (*local_dev_packages)[*local_dev_count].version = pkg->version;
    (*local_dev_count)++;

    return true;
}

int download_all_packages(ElmJson *elm_json, InstallEnv *env) {
    log_debug("Downloading all packages from elm.json");

    int total = 0;
    
    /* Track local-dev packages to re-insert into registry.dat if needed */
    int local_dev_count = 0;
    int local_dev_capacity = INITIAL_SMALL_CAPACITY;
    LocalDevPackageInfo *local_dev_packages = arena_malloc(local_dev_capacity * sizeof(LocalDevPackageInfo));
    if (!local_dev_packages) {
        log_error("Failed to allocate memory for local-dev package tracking");
        return 1;
    }

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        total = elm_json->dependencies_direct->count +
                elm_json->dependencies_indirect->count +
                elm_json->dependencies_test_direct->count +
                elm_json->dependencies_test_indirect->count;

        log_debug("Checking %d packages", total);

        // Download direct dependencies
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            
            /* Track if this is a local-dev package */
            track_if_local_dev(pkg, &local_dev_packages, &local_dev_count, &local_dev_capacity);
            
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                user_message("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s %s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s %s already cached", pkg->author, pkg->name, pkg->version);
            }
        }

        // Download indirect dependencies
        for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_indirect->packages[i];
            
            /* Track if this is a local-dev package */
            track_if_local_dev(pkg, &local_dev_packages, &local_dev_count, &local_dev_capacity);
            
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                user_message("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s %s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s %s already cached", pkg->author, pkg->name, pkg->version);
            }
        }

        // Download test direct dependencies
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            
            /* Track if this is a local-dev package */
            track_if_local_dev(pkg, &local_dev_packages, &local_dev_count, &local_dev_capacity);
            
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                user_message("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s %s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s %s already cached", pkg->author, pkg->name, pkg->version);
            }
        }

        // Download test indirect dependencies
        for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
            
            /* Track if this is a local-dev package */
            track_if_local_dev(pkg, &local_dev_packages, &local_dev_count, &local_dev_capacity);
            
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                user_message("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s %s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s %s already cached", pkg->author, pkg->name, pkg->version);
            }
        }
    } else {
        // Package project - dependencies use version constraints like "1.0.0 <= v < 2.0.0"
        total = elm_json->package_dependencies->count +
                elm_json->package_test_dependencies->count;

        log_debug("Checking %d packages", total);

        // Download package dependencies
        for (int i = 0; i < elm_json->package_dependencies->count; i++) {
            Package *pkg = &elm_json->package_dependencies->packages[i];
            
            // Resolve version constraint to actual version
            Version resolved_version;
            char *version_str;
            if (registry_is_version_constraint(pkg->version)) {
                if (!registry_resolve_constraint(env->registry, pkg->author, pkg->name, 
                                                  pkg->version, &resolved_version)) {
                    log_error("Failed to resolve version constraint for %s/%s: %s", 
                              pkg->author, pkg->name, pkg->version);
                    return 1;
                }
                version_str = version_to_string(&resolved_version);
            } else {
                version_str = pkg->version;
            }
            
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, version_str)) {
                user_message("Downloading %s/%s %s\n", pkg->author, pkg->name, version_str);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, version_str)) {
                    log_error("Failed to download %s/%s %s", pkg->author, pkg->name, version_str);
                    if (version_str != pkg->version) arena_free(version_str);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s %s already cached", pkg->author, pkg->name, version_str);
            }
            if (version_str != pkg->version) arena_free(version_str);
        }

        // Download test dependencies
        for (int i = 0; i < elm_json->package_test_dependencies->count; i++) {
            Package *pkg = &elm_json->package_test_dependencies->packages[i];
            
            // Resolve version constraint to actual version
            Version resolved_version;
            char *version_str;
            if (registry_is_version_constraint(pkg->version)) {
                if (!registry_resolve_constraint(env->registry, pkg->author, pkg->name, 
                                                  pkg->version, &resolved_version)) {
                    log_error("Failed to resolve version constraint for %s/%s: %s", 
                              pkg->author, pkg->name, pkg->version);
                    return 1;
                }
                version_str = version_to_string(&resolved_version);
            } else {
                version_str = pkg->version;
            }
            
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, version_str)) {
                user_message("Downloading %s/%s %s\n", pkg->author, pkg->name, version_str);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, version_str)) {
                    log_error("Failed to download %s/%s %s", pkg->author, pkg->name, version_str);
                    if (version_str != pkg->version) arena_free(version_str);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s %s already cached", pkg->author, pkg->name, version_str);
            }
            if (version_str != pkg->version) arena_free(version_str);
        }
    }

    /* Re-insert local-dev packages into registry.dat if they were found */
    if (local_dev_count > 0) {
        log_debug("Re-inserting %d local-dev package(s) into registry.dat", local_dev_count);
        
        /* Load the current registry.dat */
        if (env->cache && env->cache->registry_path) {
            Registry *registry = registry_load_from_dat(env->cache->registry_path, NULL);
            if (registry) {
                bool registry_modified = false;
                
                for (int i = 0; i < local_dev_count; i++) {
                    Version parsed_version = version_parse(local_dev_packages[i].version);
                    bool added = false;
                    
                    if (registry_add_version_ex(registry, local_dev_packages[i].author, 
                                               local_dev_packages[i].name, parsed_version, 
                                               false, &added)) {
                        if (added) {
                            log_debug("Re-inserted local-dev package: %s/%s %s", 
                                     local_dev_packages[i].author, 
                                     local_dev_packages[i].name, 
                                     local_dev_packages[i].version);
                            registry_modified = true;
                        }
                    } else {
                        log_error("Failed to re-insert local-dev package: %s/%s %s",
                                 local_dev_packages[i].author,
                                 local_dev_packages[i].name,
                                 local_dev_packages[i].version);
                    }
                }
                
                if (registry_modified) {
                    registry_sort_entries(registry);
                    if (!registry_dat_write(registry, env->cache->registry_path)) {
                        log_error("Failed to write registry.dat with local-dev packages");
                    } else {
                        log_debug("Successfully updated registry.dat with local-dev packages");
                    }
                }
                
                registry_free(registry);
            } else {
                log_error("Failed to load registry.dat for local-dev package re-insertion");
            }
        }
    }
    
    arena_free(local_dev_packages);

    log_debug("All dependencies downloaded successfully");
    return 0;
}

static int run_compiler_make_capture_stdout_in_dir(
    const char *compiler_path,
    char **compiler_env,
    char **argv,
    const char *cwd,
    char **out_stdout
) {
    if (out_stdout) *out_stdout = NULL;
    if (!compiler_path || !compiler_env || !argv || !cwd) return 1;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        if (chdir(cwd) != 0) {
            _exit(127);
        }
        execve(compiler_path, argv, compiler_env);
        _exit(127);
    }

    close(pipefd[1]);

    size_t cap = (size_t)MAX_ELM_JSON_FILE_BYTES;
    size_t len = 0;
    char *buf = arena_malloc(cap + 1);
    if (!buf) {
        close(pipefd[0]);
        return 1;
    }

    while (len < cap) {
        ssize_t n = read(pipefd[0], buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(pipefd[0]);

    int status = 0;
    if (pid > 0) {
        (void)waitpid(pid, &status, 0);
    }

    if (out_stdout) {
        *out_stdout = buf;
    }

    if (pid < 0) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

bool elm_cmd_run_silent_package_build(
    const char *project_dir_abs,
    const char *elm_json_path_abs,
    char **exposed_modules,
    int exposed_count,
    char **out_compiler_stdout
) {
    if (out_compiler_stdout) *out_compiler_stdout = NULL;
    if (!project_dir_abs || !elm_json_path_abs) return false;

    /* Silence stdout during dependency download + artifact cleanup */
    int saved_stdout = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (saved_stdout >= 0 && devnull >= 0) {
        (void)dup2(devnull, STDOUT_FILENO);
    }

    InstallEnv *env = install_env_create();
    if (!env || !install_env_init(env)) {
        if (devnull >= 0) close(devnull);
        if (saved_stdout >= 0) {
            (void)dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (env) install_env_free(env);
        return false;
    }

    ElmJson *elm_json = elm_json_read(elm_json_path_abs);
    if (!elm_json) {
        install_env_free(env);
        if (devnull >= 0) close(devnull);
        if (saved_stdout >= 0) {
            (void)dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        return false;
    }

    int dl_result = download_all_packages(elm_json, env);
    builder_clean_local_dev_artifacts(elm_json_path_abs, env->cache);
    elm_json_free(elm_json);

    /* Restore stdout */
    if (devnull >= 0) close(devnull);
    if (saved_stdout >= 0) {
        (void)dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }

    if (dl_result != 0) {
        install_env_free(env);
        return false;
    }

    char *compiler_path = elm_compiler_get_path();
    if (!compiler_path) {
        install_env_free(env);
        return false;
    }

    char **compiler_env = build_elm_environment();
    if (!compiler_env) {
        install_env_free(env);
        return false;
    }

    const char *compiler_name = global_context_compiler_name();
    bool ok = true;
    char *last_stdout = NULL;

    if (exposed_modules && exposed_count > 0) {
        for (int i = 0; i < exposed_count; i++) {
            const char *module_name = exposed_modules[i];
            if (!module_name) continue;

            char *rel_elm = elm_module_name_to_path(module_name, "src");
            if (!rel_elm) continue;

            char abs_elm[MAX_PATH_LENGTH];
            snprintf(abs_elm, sizeof(abs_elm), "%s/%s", project_dir_abs, rel_elm);
            if (!file_exists(abs_elm)) {
                continue;
            }

            char *out_path = arena_strdup("/dev/null");

            char **elm_args = arena_malloc(sizeof(char*) * 7);
            elm_args[0] = (char*)compiler_name;
            elm_args[1] = "make";
            elm_args[2] = "--report=json";
            elm_args[3] = rel_elm;
            elm_args[4] = "--output";
            elm_args[5] = out_path;
            elm_args[6] = NULL;

            char *compiler_stdout = NULL;
            int exit_code = run_compiler_make_capture_stdout_in_dir(
                compiler_path,
                compiler_env,
                elm_args,
                project_dir_abs,
                &compiler_stdout
            );

            last_stdout = compiler_stdout;
            if (exit_code != 0) {
                ok = false;
                break;
            }
        }
    }

    if (out_compiler_stdout) {
        *out_compiler_stdout = last_stdout;
    }

    /* Always remove elm-stuff afterwards */
    char elm_stuff_path[MAX_PATH_LENGTH];
    snprintf(elm_stuff_path, sizeof(elm_stuff_path), "%s/elm-stuff", project_dir_abs);
    if (file_exists(elm_stuff_path)) {
        (void)remove_directory_recursive(elm_stuff_path);
    }

    install_env_free(env);
    return ok;
}
