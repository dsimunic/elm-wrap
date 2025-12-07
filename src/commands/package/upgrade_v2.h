/**
 * upgrade_v2.h - V2 Protocol Package Upgrade Functions
 *
 * Functions for package upgrade using the V2 protocol.
 * These functions use the V2 registry index - all data is in memory,
 * no network access needed for dependency checking.
 */

#ifndef UPGRADE_V2_H
#define UPGRADE_V2_H

#include "../../install_env.h"
#include "../../elm_json.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include <stdbool.h>

/**
 * Upgrade a single package using V2 protocol.
 * Uses V2 registry index - all dependency data is in memory.
 *
 * @param package Package name in "author/name" format
 * @param elm_json Current project elm.json
 * @param env Install environment with V2 registry
 * @param major_upgrade Allow major version upgrades
 * @param major_ignore_test Ignore test dependency conflicts for major upgrades
 * @param auto_yes Automatically confirm changes without prompting
 * @return 0 on success, non-zero on error
 */
int upgrade_single_package_v2(const char *package, ElmJson *elm_json, InstallEnv *env,
                              bool major_upgrade, bool major_ignore_test, bool auto_yes);

/**
 * Upgrade all packages using V2 protocol.
 *
 * @param elm_json Current project elm.json
 * @param env Install environment with V2 registry
 * @param major_upgrade Allow major version upgrades
 * @param major_ignore_test Ignore test dependency conflicts for major upgrades
 * @param auto_yes Automatically confirm changes without prompting
 * @return 0 on success, non-zero on error
 */
int upgrade_all_packages_v2(ElmJson *elm_json, InstallEnv *env,
                            bool major_upgrade, bool major_ignore_test, bool auto_yes);

#endif /* UPGRADE_V2_H */
