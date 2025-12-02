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
 * Handle package deps command in V2 mode.
 * Loads the V2 registry and displays dependencies.
 *
 * @param package_arg Package name in "author/name" format
 * @param version_arg Optional version string, or NULL for latest/elm.json version
 * @return 0 on success, 1 on error
 */
int v2_cmd_deps(const char *package_arg, const char *version_arg);

#endif /* PROTOCOL_V2_INSTALL_H */
