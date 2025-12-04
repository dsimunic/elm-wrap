/**
 * protocol_v2/install.c - V2 Protocol Install Functions Implementation
 *
 * Functions for package dependency display using the V2 protocol.
 * These functions do not require network access - all data is in the registry index.
 */

#include "install.h"
#include "../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
