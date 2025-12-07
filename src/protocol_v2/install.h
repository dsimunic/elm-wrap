/**
 * protocol_v2/install.h - V2 Protocol Install Functions
 *
 * Functions for package dependency display using the V2 protocol.
 * These functions do not require network access - all data is in the registry index.
 */

#ifndef PROTOCOL_V2_INSTALL_H
#define PROTOCOL_V2_INSTALL_H

#include "solver/v2_registry.h"

/**
 * Show package dependencies using V2 registry data.
 * No network access or package download required - all data is in the registry index.
 *
 * @param author Package author
 * @param name Package name
 * @param version Package version string (e.g., "1.0.0")
 * @param registry V2 registry with package data
 * @return 0 on success, 1 on error
 */
int v2_show_package_dependencies(const char *author, const char *name, const char *version,
                                 V2Registry *registry);

/**
 * Check if a package depends on another package using V2 registry data.
 * All data is available in the registry index - no downloads needed.
 *
 * @param pkg_author Package author to check
 * @param pkg_name Package name to check
 * @param pkg_version Package version string (e.g., "1.0.0")
 * @param target_author Dependency target author
 * @param target_name Dependency target name
 * @param registry V2 registry with package data
 * @return true if pkg depends on target, false otherwise
 */
bool v2_package_depends_on(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                           const char *target_author, const char *target_name,
                           V2Registry *registry);

#endif /* PROTOCOL_V2_INSTALL_H */
