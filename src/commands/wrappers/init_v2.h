#ifndef WRAPPER_INIT_V2_H
#define WRAPPER_INIT_V2_H

#include <stdbool.h>
#include "../../elm_json.h"

struct InstallEnv;

/**
 * V2 Protocol: Solve dependencies for elm init
 *
 * Creates direct and indirect dependency maps for elm/browser, elm/core, and elm/html
 * using the V2 registry.
 *
 * @param env Install environment with V2 registry loaded
 * @param direct_deps Output parameter for direct dependencies
 * @param indirect_deps Output parameter for indirect dependencies
 * @return true on success, false on failure
 */
bool solve_init_dependencies_v2(
    struct InstallEnv *env,
    PackageMap **direct_deps,
    PackageMap **indirect_deps
);

#endif /* WRAPPER_INIT_V2_H */
