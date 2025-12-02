/**
 * protocol_v2/install.c - V2 Protocol Install Functions Implementation
 *
 * Functions for package dependency display using the V2 protocol.
 * These functions do not require network access - all data is in the registry index.
 */

#include "install.h"
#include "../elm_json.h"
#include "../global_context.h"
#include "../log.h"
#include "../alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define ELM_JSON_PATH "elm.json"

/**
 * Parse a package name in "author/name" format.
 */
static bool parse_package_name(const char *package, char **author, char **name) {
    if (!package) return false;

    const char *slash = strchr(package, '/');
    if (!slash) {
        fprintf(stderr, "Error: Package name must be in format 'author/package'\n");
        return false;
    }

    size_t author_len = slash - package;
    *author = arena_malloc(author_len + 1);
    if (!*author) return false;
    strncpy(*author, package, author_len);
    (*author)[author_len] = '\0';

    *name = arena_strdup(slash + 1);
    if (!*name) {
        arena_free(*author);
        return false;
    }

    return true;
}

/**
 * Find an existing package in elm.json (both application and package formats).
 */
static Package* find_existing_package(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || !author || !name) {
        return NULL;
    }

    Package *pkg = NULL;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        pkg = package_map_find(elm_json->dependencies_direct, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_indirect, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_test_direct, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_test_indirect, author, name);
        if (pkg) return pkg;
    } else {
        pkg = package_map_find(elm_json->package_dependencies, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->package_test_dependencies, author, name);
        if (pkg) return pkg;
    }

    return NULL;
}

int v2_show_package_dependencies(const char *author, const char *name, const char *version,
                                 V2Registry *registry) {
    /* Parse version string to components */
    int major, minor, patch;
    if (sscanf(version, "%d.%d.%d", &major, &minor, &patch) != 3) {
        log_error("Invalid version format: %s", version);
        return 1;
    }

    /* Find the package version in the registry */
    V2PackageVersion *pkg_version = v2_registry_find_version(registry, author, name,
                                                             (uint16_t)major, (uint16_t)minor, (uint16_t)patch);
    if (!pkg_version) {
        log_error("Version %s not found for package %s/%s in V2 registry", version, author, name);
        return 1;
    }

    printf("\n");
    printf("Package: %s/%s @ %s\n", author, name, version);
    printf("========================================\n\n");

    if (pkg_version->dependency_count == 0) {
        printf("No dependencies\n");
    } else {
        /* Calculate max width for alignment */
        int max_width = 0;
        for (size_t i = 0; i < pkg_version->dependency_count; i++) {
            V2Dependency *dep = &pkg_version->dependencies[i];
            if (dep && dep->package_name) {
                int pkg_len = (int)strlen(dep->package_name);
                if (pkg_len > max_width) max_width = pkg_len;
            }
        }

        printf("Dependencies (%zu):\n", pkg_version->dependency_count);
        for (size_t i = 0; i < pkg_version->dependency_count; i++) {
            V2Dependency *dep = &pkg_version->dependencies[i];
            if (dep && dep->package_name && dep->constraint) {
                printf("  %-*s %s\n", max_width, dep->package_name, dep->constraint);
            } else {
                printf("  [corrupted dependency %zu]\n", i);
            }
        }
    }

    printf("\n");
    return 0;
}

int v2_cmd_deps(const char *package_arg, const char *version_arg) {
    GlobalContext *ctx = global_context_get();
    
    /* Build path to index.dat in V2 repository */
    size_t index_path_len = strlen(ctx->repository_path) + strlen("/index.dat") + 1;
    char *index_path = arena_malloc(index_path_len);
    if (!index_path) {
        log_error("Failed to allocate memory for index path");
        return 1;
    }
    snprintf(index_path, index_path_len, "%s/index.dat", ctx->repository_path);
    
    log_debug("Using V2 registry from: %s", index_path);
    
    /* Load V2 registry from zip file */
    V2Registry *v2_registry = v2_registry_load_from_zip(index_path);
    if (!v2_registry) {
        log_error("Failed to load V2 registry from %s", index_path);
        arena_free(index_path);
        return 1;
    }
    
    arena_free(index_path);
    log_debug("Loaded V2 registry with %zu packages", v2_registry->entry_count);

    /* Parse package name */
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package_arg, &author, &name)) {
        v2_registry_free(v2_registry);
        return 1;
    }

    /* Find package in V2 registry */
    V2PackageEntry *pkg_entry = v2_registry_find(v2_registry, author, name);
    if (!pkg_entry) {
        log_error("I cannot find package '%s/%s'", author, name);
        log_error("Make sure the package name is correct");
        arena_free(author);
        arena_free(name);
        v2_registry_free(v2_registry);
        return 1;
    }

    const char *version_to_use = NULL;
    bool version_found = false;

    if (version_arg) {
        /* User specified a version - check if it exists */
        int major, minor, patch;
        if (sscanf(version_arg, "%d.%d.%d", &major, &minor, &patch) == 3) {
            for (size_t i = 0; i < pkg_entry->version_count; i++) {
                V2PackageVersion *v = &pkg_entry->versions[i];
                if (v->major == (uint16_t)major && 
                    v->minor == (uint16_t)minor && 
                    v->patch == (uint16_t)patch) {
                    version_to_use = version_arg;
                    version_found = true;
                    break;
                }
            }
        }

        if (!version_found) {
            log_error("Version %s not found for package %s/%s", version_arg, author, name);
            printf("\nAvailable versions:\n");
            for (size_t i = 0; i < pkg_entry->version_count; i++) {
                V2PackageVersion *v = &pkg_entry->versions[i];
                if (v->status == V2_STATUS_VALID) {
                    printf("  %u.%u.%u\n", v->major, v->minor, v->patch);
                }
            }
            printf("\n");
            arena_free(author);
            arena_free(name);
            v2_registry_free(v2_registry);
            return 1;
        }
    } else {
        /* Try to find version from current elm.json */
        ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
        if (elm_json) {
            Package *existing_pkg = find_existing_package(elm_json, author, name);
            if (existing_pkg && existing_pkg->version) {
                /* For application elm.json, version is a semver like "1.0.0"
                 * For package elm.json, version is a constraint like "1.0.0 <= v < 2.0.0"
                 * Only use the version if it's a proper semver (contains no spaces) */
                if (!strchr(existing_pkg->version, ' ')) {
                    version_to_use = arena_strdup(existing_pkg->version);
                    version_found = true;
                    log_debug("Using version %s from elm.json", version_to_use);
                }
            }
            elm_json_free(elm_json);
        }

        /* If not in elm.json or it's a constraint, use latest valid version from registry */
        if (!version_found && pkg_entry->version_count > 0) {
            /* Versions are stored newest first, find first valid one */
            for (size_t i = 0; i < pkg_entry->version_count; i++) {
                V2PackageVersion *v = &pkg_entry->versions[i];
                if (v->status == V2_STATUS_VALID) {
                    char *latest = arena_malloc(32);
                    snprintf(latest, 32, "%u.%u.%u", v->major, v->minor, v->patch);
                    version_to_use = latest;
                    version_found = true;
                    log_debug("Using latest version %s from registry", version_to_use);
                    break;
                }
            }
        }
    }

    if (!version_found || !version_to_use) {
        log_error("Could not determine version for %s/%s", author, name);
        arena_free(author);
        arena_free(name);
        v2_registry_free(v2_registry);
        return 1;
    }

    int result = v2_show_package_dependencies(author, name, version_to_use, v2_registry);

    /* Clean up version_to_use if we allocated it */
    if (version_to_use != version_arg && version_found && !version_arg) {
        ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
        if (!elm_json || !find_existing_package(elm_json, author, name)) {
            arena_free((char*)version_to_use);
        }
        if (elm_json) {
            elm_json_free(elm_json);
        }
    }

    arena_free(author);
    arena_free(name);
    v2_registry_free(v2_registry);
    return result;
}
