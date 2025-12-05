#ifndef WRAPPER_INIT_V1_H
#define WRAPPER_INIT_V1_H

#include <stdbool.h>
#include "../../elm_json.h"

struct InstallEnv;

/**
 * V1 Protocol: Solve dependencies for elm init
 *
 * Creates direct and indirect dependency maps for elm/browser, elm/core, and elm/html
 * using the V1 registry.
 *
 * @param env Install environment with V1 registry loaded
 * @param direct_deps Output parameter for direct dependencies
 * @param indirect_deps Output parameter for indirect dependencies
 * @return true on success, false on failure
 */
bool solve_init_dependencies_v1(
    struct InstallEnv *env,
    PackageMap **direct_deps,
    PackageMap **indirect_deps
);

#endif /* WRAPPER_INIT_V1_H */
