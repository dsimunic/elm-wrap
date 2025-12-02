#ifndef INSTALL_CHECK_H
#define INSTALL_CHECK_H

#include "elm_json.h"
#include "registry.h"
#include "protocol_v2/solver/v2_registry.h"
#include <stdbool.h>

/* Check for available package upgrades (V1 protocol)
 * Returns 0 if upgrades found, 100 if no upgrades available
 */
int check_all_upgrades(const char *elm_json_path, Registry *registry);

/* Check for available package upgrades (V2 protocol)
 * Uses the V2 registry index which contains all dependency information.
 * Returns 0 if upgrades found, 100 if no upgrades available
 */
int check_all_upgrades_v2(const char *elm_json_path, V2Registry *registry);

#endif /* INSTALL_CHECK_H */
