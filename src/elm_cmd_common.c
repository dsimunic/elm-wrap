/**
 * elm_cmd_common.c - Common utilities for Elm command wrappers
 */

#include "elm_cmd_common.h"
#include "cache.h"
#include "registry.h"
#include "alloc.h"
#include "log.h"
#include <stdlib.h>
#include <stdio.h>

char **build_elm_environment(void) {
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

int download_all_packages(ElmJson *elm_json, InstallEnv *env) {
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
