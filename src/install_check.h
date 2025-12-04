#ifndef INSTALL_CHECK_H
#define INSTALL_CHECK_H

#include "elm_json.h"
#include "registry.h"
#include "protocol_v2/solver/v2_registry.h"
#include <stdbool.h>

/* Check for available package upgrades (V1 protocol)
 * Returns 0 if upgrades found, 100 if no upgrades available
 * max_name_len: maximum package name length for alignment (0 = auto-calculate)
 */
int check_all_upgrades(const char *elm_json_path, Registry *registry, size_t max_name_len);

/* Check for available package upgrades (V2 protocol)
 * Uses the V2 registry index which contains all dependency information.
 * Returns 0 if upgrades found, 100 if no upgrades available
 * max_name_len: maximum package name length for alignment (0 = auto-calculate)
 */
int check_all_upgrades_v2(const char *elm_json_path, V2Registry *registry, size_t max_name_len);

/* Get the maximum package name length among available upgrades (V1 protocol)
 * Returns 0 if no upgrades available or on error.
 * Use this to calculate alignment before printing installed packages.
 */
size_t get_max_upgrade_name_len(const char *elm_json_path, Registry *registry);

/* Get the maximum package name length among available upgrades (V2 protocol)
 * Returns 0 if no upgrades available or on error.
 * Use this to calculate alignment before printing installed packages.
 */
size_t get_max_upgrade_name_len_v2(const char *elm_json_path, V2Registry *registry);

#endif /* INSTALL_CHECK_H */
