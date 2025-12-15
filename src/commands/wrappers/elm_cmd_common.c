/**
 * elm_cmd_common.c - Common utilities for Elm command wrappers
 */

#include "elm_cmd_common.h"
#include "../../cache.h"
#include "../../registry.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../constants.h"
#include "../../commands/package/package_common.h"
#include "../../local_dev/local_dev_tracking.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, pkg->version);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, version_str);
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
                printf("Downloading %s/%s %s\n", pkg->author, pkg->name, version_str);
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
