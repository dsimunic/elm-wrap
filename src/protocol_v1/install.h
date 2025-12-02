/**
 * protocol_v1/install.h - V1 Protocol Install Functions
 *
 * Functions for package dependency display using the V1 protocol.
 * These functions require network access and package downloads.
 */

#ifndef PROTOCOL_V1_INSTALL_H
#define PROTOCOL_V1_INSTALL_H

#include "../install_env.h"

/**
 * Check if a package depends on another package.
 * Uses cache to read package elm.json, downloading if necessary.
 *
 * @param pkg_author Package author to check
 * @param pkg_name Package name to check
 * @param pkg_version Package version to check
 * @param target_author Dependency target author
 * @param target_name Dependency target name
 * @param env Install environment with cache access
 * @return true if pkg depends on target, false otherwise
 */
bool v1_package_depends_on(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                           const char *target_author, const char *target_name,
                           InstallEnv *env);

/**
 * Show package dependencies using V1 protocol.
 * Downloads and reads package elm.json from cache.
 *
 * @param author Package author
 * @param name Package name
 * @param version Package version
 * @param env Install environment with cache access
 * @return 0 on success, 1 on error
 */
int v1_show_package_dependencies(const char *author, const char *name, const char *version,
                                 InstallEnv *env);

#endif /* PROTOCOL_V1_INSTALL_H */
