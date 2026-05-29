/**
 * protocol_v2/install.c - V2 Protocol Install Functions Implementation
 *
 * Functions for package dependency display using the V2 protocol.
 * These functions do not require network access - all data is in the registry index.
 */

#include "install.h"
#include "../shared/log.h"
#include "../constants.h"
#include "../commands/package/package_common.h"
#include "../alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

int v2_show_package_dependencies(const char *author, const char *name, const char *version,
                                 V2Registry *registry) {
    /* Parse version string to components */
    Version parsed_v;
    if (!version_parse_safe(version, &parsed_v)) {
        log_error("Invalid version format: %s", version);
        return 1;
    }

    /* Find the package version in the registry */
    V2PackageVersion *pkg_version = v2_registry_find_version(registry, author, name,
                                                             parsed_v.major, parsed_v.minor, parsed_v.patch);
    if (!pkg_version) {
        log_error("Version %s not found for package %s/%s in V2 registry", version, author, name);
        return 1;
    }

    printf("\n");
    printf("Package: %s/%s %s\n", author, name, version);
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

char *v2_package_dependency_constraint(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                                       const char *target_author, const char *target_name,
                                       V2Registry *registry) {
    if (!registry || !pkg_author || !pkg_name || !pkg_version ||
        !target_author || !target_name) {
        return NULL;
    }

    /* Parse the version string */
    Version parsed_v;
    if (!version_parse_safe(pkg_version, &parsed_v)) {
        log_debug("Invalid version format: %s", pkg_version);
        return NULL;
    }

    /* Find the specific version in the registry */
    V2PackageVersion *version = v2_registry_find_version(registry, pkg_author, pkg_name,
                                                          parsed_v.major, parsed_v.minor, parsed_v.patch);
    if (!version) {
        log_debug("Version %s not found for %s/%s in V2 registry", pkg_version, pkg_author, pkg_name);
        return NULL;
    }

    /* Build the target package name for comparison */
    char target_full_name[MAX_PACKAGE_NAME_LENGTH];
    snprintf(target_full_name, sizeof(target_full_name), "%s/%s", target_author, target_name);

    /* Return the constraint of the dependency matching the target, if any */
    for (size_t i = 0; i < version->dependency_count; i++) {
        V2Dependency *dep = &version->dependencies[i];
        if (dep && dep->package_name && strcmp(dep->package_name, target_full_name) == 0) {
            return dep->constraint ? arena_strdup(dep->constraint) : NULL;
        }
    }

    return NULL;
}

bool v2_package_depends_on(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                           const char *target_author, const char *target_name,
                           V2Registry *registry) {
    char *constraint = v2_package_dependency_constraint(pkg_author, pkg_name, pkg_version,
                                                        target_author, target_name, registry);
    bool depends = (constraint != NULL);
    arena_free(constraint);
    return depends;
}
