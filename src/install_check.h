#ifndef INSTALL_CHECK_H
#define INSTALL_CHECK_H

#include "elm_json.h"
#include "registry.h"
#include <stdbool.h>

/* Check for available package upgrades
 * Returns 0 if upgrades found, 100 if no upgrades available
 */
int check_all_upgrades(const char *elm_json_path, Registry *registry);

#endif /* INSTALL_CHECK_H */
