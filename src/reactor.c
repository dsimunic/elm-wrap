#include "reactor.h"
#include "elm_json.h"
#include "cache.h"
#include "install_env.h"
#include "registry.h"
#include "alloc.h"
#include "log.h"
#include "progname.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ELM_JSON_PATH "elm.json"

static void print_reactor_usage(void) {
    printf("Usage: %s reactor [OPTIONS]\n", program_name);
    printf("\n");
    printf("Start the Elm Reactor development server.\n");
    printf("\n");
    printf("This command ensures all package dependencies are downloaded and cached\n");
    printf("before calling 'elm reactor'.\n");
    printf("\n");
    printf("All options are passed through to 'elm reactor'.\n");
}

// Search for elm binary in PATH
// Returns NULL if not found
static char *find_elm_binary(void) {
    const char *path_env = getenv("PATH");
    if (!path_env) {
        return NULL;
    }

    // Make a copy of PATH since strtok modifies the string
    char *path_copy = arena_strdup(path_env);
    if (!path_copy) {
        return NULL;
    }

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        char *candidate = arena_malloc(PATH_MAX);
        if (!candidate) {
            return NULL;
        }

        snprintf(candidate, PATH_MAX, "%s/elm", dir);

        // Check if file exists and is executable
        struct stat st;
        if (stat(candidate, &st) == 0 && (st.st_mode & S_IXUSR)) {
            return candidate;
        }

        dir = strtok(NULL, ":");
    }

    return NULL;
}

// Get the elm compiler path from environment variable or search PATH
static char *get_elm_compiler_path(void) {
    const char *compiler_path = getenv("ELM_WRAP_ELM_COMPILER_PATH");
    if (compiler_path) {
        return arena_strdup(compiler_path);
    }

    return find_elm_binary();
}

// Build environment array with current environment plus https_proxy
// Returns NULL if memory allocation fails
static char **build_elm_environment(void) {
    extern char **environ;

    // Check if we should keep elm online
    const char *keep_online = getenv("ELM_WRAP_ALLOW_ELM_ONLINE");

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

static int download_all_packages(ElmJson *elm_json, InstallEnv *env) {
    log_debug("Downloading all packages from elm.json");

    int total = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        total = elm_json->dependencies_direct->count +
                elm_json->dependencies_indirect->count +
                elm_json->dependencies_test_direct->count +
                elm_json->dependencies_test_indirect->count;

        log_debug("Checking %d packages", total);

        // Download direct dependencies
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s@%s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s@%s already cached", pkg->author, pkg->name, pkg->version);
            }
        }

        // Download indirect dependencies
        for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_indirect->packages[i];
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s@%s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s@%s already cached", pkg->author, pkg->name, pkg->version);
            }
        }

        // Download test direct dependencies
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s@%s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s@%s already cached", pkg->author, pkg->name, pkg->version);
            }
        }

        // Download test indirect dependencies
        for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
            if (!cache_package_exists(env->cache, pkg->author, pkg->name, pkg->version)) {
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, pkg->version)) {
                    log_error("Failed to download %s/%s@%s", pkg->author, pkg->name, pkg->version);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s@%s already cached", pkg->author, pkg->name, pkg->version);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, version_str);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, version_str)) {
                    log_error("Failed to download %s/%s@%s", pkg->author, pkg->name, version_str);
                    if (version_str != pkg->version) arena_free(version_str);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s@%s already cached", pkg->author, pkg->name, version_str);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, version_str);
                if (!cache_download_package_with_env(env, pkg->author, pkg->name, version_str)) {
                    log_error("Failed to download %s/%s@%s", pkg->author, pkg->name, version_str);
                    if (version_str != pkg->version) arena_free(version_str);
                    return 1;
                }
            } else {
                log_debug("Package %s/%s@%s already cached", pkg->author, pkg->name, version_str);
            }
            if (version_str != pkg->version) arena_free(version_str);
        }
    }

    log_debug("All dependencies downloaded successfully");
    return 0;
}

int cmd_reactor(int argc, char *argv[]) {
    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_reactor_usage();
            return 0;
        }
    }

    // Initialize environment
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        return 1;
    }

    log_debug("ELM_HOME: %s", env->cache->elm_home);

    // Read elm.json
    log_debug("Reading elm.json");
    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        install_env_free(env);
        return 1;
    }

    // Download all packages
    int result = download_all_packages(elm_json, env);

    // Cleanup
    elm_json_free(elm_json);
    install_env_free(env);

    if (result != 0) {
        log_error("Failed to download all dependencies");
        return result;
    }

    // Now call elm reactor with all the arguments
    printf("\nAll dependencies cached. Running elm reactor...\n\n");

    // Get elm compiler path
    char *elm_path = get_elm_compiler_path();
    if (!elm_path) {
        log_error("Could not find elm binary");
        log_error("Please install elm or set the ELM_WRAP_ELM_COMPILER_PATH environment variable");
        return 1;
    }

    log_debug("Using elm compiler at: %s", elm_path);

    // Build environment with https_proxy for offline mode
    char **elm_env = build_elm_environment();
    if (!elm_env) {
        log_error("Failed to build environment for elm");
        return 1;
    }

    // Build elm reactor command
    // We need to pass all arguments except "reactor" to elm
    char **elm_args = arena_malloc(sizeof(char*) * (argc + 2));
    elm_args[0] = "elm";
    elm_args[1] = "reactor";

    // Copy remaining arguments
    for (int i = 1; i < argc; i++) {
        elm_args[i + 1] = argv[i];
    }
    elm_args[argc + 1] = NULL;

    // Execute elm reactor with custom environment
    execve(elm_path, elm_args, elm_env);

    // If execve returns, it failed
    log_error("Failed to execute elm compiler at: %s", elm_path);
    if (getenv("ELM_WRAP_ELM_COMPILER_PATH")) {
        log_error("The compiler was not found at the path specified in ELM_WRAP_ELM_COMPILER_PATH");
    }
    perror("execve");
    arena_free(elm_args);
    return 1;
}
