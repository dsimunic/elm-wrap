#include "solver.h"
#include "install_env.h"
#include "pgsolver/solver_common.h"
#include "protocol_v1/solver/solver.h"
#include "protocol_v2/solver/solver.h"
#include "cache.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/* Helper function to run solver with a specific strategy */
static SolverResult run_with_strategy(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    bool upgrade_all,
    SolverStrategy strategy,
    PackageMap *current_packages,
    InstallPlan **out_plan
) {
    /* Dispatch to protocol-specific implementation */
    bool is_v2 = (state->install_env && state->install_env->protocol_mode == PROTOCOL_V2);
    if (is_v2) {
        return run_with_strategy_v2(state, elm_json, author, name, is_test_dependency,
                                     upgrade_all, strategy, current_packages, out_plan);
    } else {
        return run_with_strategy_v1(state, elm_json, author, name, is_test_dependency,
                                     upgrade_all, strategy, current_packages, out_plan);
    }
}

/* Main solver function */
SolverResult solver_add_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    bool major_upgrade,
    bool upgrade_all,
    InstallPlan **out_plan
) {
    if (!state || !elm_json || !author || !name || !out_plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    *out_plan = NULL;

    log_debug("Adding package: %s/%s%s%s",
            author, name, is_test_dependency ? " (test dependency)" : "",
            major_upgrade ? " (major upgrade allowed)" : "");

    /* Collect current packages */
    PackageMap *current_packages = collect_current_packages(elm_json);
    if (!current_packages) {
        return SOLVER_INVALID_PACKAGE;
    }

    /* Check if package needs to be downloaded from registry */
    if (!state->online && !cache_registry_exists(state->cache)) {
        log_error("Offline mode but no cached registry");
        package_map_free(current_packages);
        return SOLVER_NO_OFFLINE_SOLUTION;
    }

    /* Ensure registry is available (install_env_init already fetched/updated it) */
    if (!cache_registry_exists(state->cache)) {
        log_error("Registry not available in cache after initialization");
        package_map_free(current_packages);
        return SOLVER_NETWORK_ERROR;
    }

    /* Strategy ladder: choose strategies based on major_upgrade flag */
    SolverStrategy strategies[4];
    int num_strategies;

    if (major_upgrade) {
        /* For major upgrades, only try cross-major strategy */
        strategies[0] = STRATEGY_CROSS_MAJOR_FOR_TARGET;
        num_strategies = 1;
    } else {
        /* Default: try exact, then exact-direct, then within-major */
        strategies[0] = STRATEGY_EXACT_ALL;
        strategies[1] = STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT;
        strategies[2] = STRATEGY_UPGRADABLE_WITHIN_MAJOR;
        num_strategies = 3;
    }

    SolverResult result = SOLVER_NO_SOLUTION;
    for (int i = 0; i < num_strategies; i++) {
        result = run_with_strategy(
            state,
            elm_json,
            author,
            name,
            is_test_dependency,
            upgrade_all,
            strategies[i],
            current_packages,
            out_plan
        );

        if (result == SOLVER_OK) {
            log_debug("Solution found using strategy %d", i);
            package_map_free(current_packages);
            return SOLVER_OK;
        } else if (result != SOLVER_NO_SOLUTION) {
            /* Non-solvable error (network, invalid package, etc) */
            package_map_free(current_packages);
            return result;
        }
        /* Continue to next strategy */
    }

    /* All strategies failed */
    log_error("All solver strategies failed for %s/%s", author, name);
    package_map_free(current_packages);
    return SOLVER_NO_SOLUTION;
}

/* Upgrade all packages - run solver allowing minor/major upgrades */
SolverResult solver_upgrade_all(
    SolverState *state,
    const ElmJson *elm_json,
    bool major_upgrade,
    InstallPlan **out_plan
) {
    if (!state || !elm_json || !out_plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    *out_plan = NULL;

    log_debug("Upgrading all packages%s", major_upgrade ? " (major allowed)" : "");

    /* Dispatch to protocol-specific implementation */
    bool is_v2 = (state->install_env && state->install_env->protocol_mode == PROTOCOL_V2);
    if (is_v2) {
        return solver_upgrade_all_v2(state, elm_json, major_upgrade, out_plan);
    } else {
        return solver_upgrade_all_v1(state, elm_json, major_upgrade, out_plan);
    }
}
