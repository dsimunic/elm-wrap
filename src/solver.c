#include "solver.h"
#include "install_env.h"
#include "pgsolver/solver_common.h"
#include "protocol_v1/solver/solver.h"
#include "protocol_v2/solver/solver.h"
#include "protocol_v2/solver/v2_registry.h"
#include "registry.h"
#include "cache.h"
#include "shared/log.h"
#include "alloc.h"
#include "commands/package/package_common.h"
#include <stdlib.h>
#include <string.h>

/* Helper function to run solver with a specific strategy */
static SolverResult run_with_strategy(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    const Version *target_version,
    bool is_test_dependency,
    bool upgrade_all,
    SolverStrategy strategy,
    PackageMap *current_packages,
    InstallPlan **out_plan
) {
    /* Dispatch to protocol-specific implementation */
    bool is_v2 = (state->install_env && state->install_env->protocol_mode == PROTOCOL_V2);
    if (is_v2) {
        return run_with_strategy_v2(state, elm_json, author, name, target_version,
                                     is_test_dependency, upgrade_all, strategy, current_packages, out_plan);
    } else {
        return run_with_strategy_v1(state, elm_json, author, name, target_version,
                                     is_test_dependency, upgrade_all, strategy, current_packages, out_plan);
    }
}

/* Main solver function */
SolverResult solver_add_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    const Version *target_version,
    bool is_test_dependency,
    bool major_upgrade,
    bool upgrade_all,
    InstallPlan **out_plan
) {
    if (!state || !elm_json || !author || !name || !out_plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    *out_plan = NULL;

    if (target_version) {
        log_debug("Adding package: %s/%s@%u.%u.%u%s%s",
                author, name, target_version->major, target_version->minor, target_version->patch,
                is_test_dependency ? " (test dependency)" : "",
                major_upgrade ? " (major upgrade allowed)" : "");
    } else {
        log_debug("Adding package: %s/%s%s%s",
                author, name, is_test_dependency ? " (test dependency)" : "",
                major_upgrade ? " (major upgrade allowed)" : "");
    }

    /* Collect current packages */
    PackageMap *current_packages = collect_current_packages(elm_json);
    if (!current_packages) {
        return SOLVER_INVALID_PACKAGE;
    }

    /* Check if package needs to be downloaded from registry */
    if (!state->online && !cache_registry_exists(state->cache)) {
        if (state->install_env && state->install_env->offline_forced) {
            log_error("WRAP_OFFLINE_MODE=1 prevents downloading the registry (no cached data available)");
        } else {
            log_error("Offline mode but no cached registry");
        }
        package_map_free(current_packages);
        return SOLVER_NO_OFFLINE_SOLUTION;
    }

    /* Ensure registry is available (install_env_init already fetched/updated it) */
    if (!cache_registry_exists(state->cache)) {
        log_error("Registry not available in cache after initialization");
        package_map_free(current_packages);
        return SOLVER_NETWORK_ERROR;
    }

    /* Strategy ladder: choose strategies based on target version / flags */
    SolverStrategy strategies[4];
    int num_strategies;

    if (target_version) {
        /*
         * For explicitly requested versions, keep indirect deps flexible first
         * and fall back to exact resolution if that fails.
         */
        strategies[0] = STRATEGY_UPGRADABLE_WITHIN_MAJOR;
        strategies[1] = STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT;
        strategies[2] = STRATEGY_EXACT_ALL;
        num_strategies = 3;
    } else if (major_upgrade) {
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
            target_version,
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

/* Free multi-package validation results */
void multi_package_validation_free(MultiPackageValidation *validation) {
    if (!validation) return;
    if (validation->results) {
        arena_free(validation->results);
    }
    arena_free(validation);
}


/* Check if package exists in registry (for V1 and V2 protocols) */
static bool package_exists_in_registry_internal(
    SolverState *state,
    const char *author,
    const char *name
) {
    if (!state->install_env) return false;

    if (state->install_env->protocol_mode == PROTOCOL_V2) {
        if (!state->install_env->v2_registry) return false;
        V2PackageEntry *entry = v2_registry_find(state->install_env->v2_registry, author, name);
        if (!entry) return false;
        /* Check for at least one valid version */
        for (size_t i = 0; i < entry->version_count; i++) {
            if (entry->versions[i].status == V2_STATUS_VALID) {
                return true;
            }
        }
        return false;
    } else {
        if (!state->install_env->registry) return false;
        RegistryEntry *entry = registry_find(state->install_env->registry, author, name);
        return (entry != NULL);
    }
}

/* Merge source install plan into destination */
void install_plan_merge(InstallPlan *dest, const InstallPlan *source) {
    if (!dest || !source) return;

    for (int i = 0; i < source->count; i++) {
        PackageChange *src_change = &source->changes[i];

        /* Check if this package is already in dest */
        bool found = false;
        for (int j = 0; j < dest->count; j++) {
            if (strcmp(dest->changes[j].author, src_change->author) == 0 &&
                strcmp(dest->changes[j].name, src_change->name) == 0) {
                found = true;
                break;
            }
        }

        /* Only add if not already present */
        if (!found) {
            install_plan_add_change(dest, src_change->author, src_change->name,
                                    src_change->old_version, src_change->new_version);
        }
    }
}

/* Add multiple packages to the project */
SolverResult solver_add_packages(
    SolverState *state,
    const ElmJson *elm_json,
    const PackageVersionSpec *packages,
    int count,
    bool is_test,
    bool upgrade_all,
    InstallPlan **out_plan,
    MultiPackageValidation **out_validation
) {
    if (!state || !elm_json || !packages || count <= 0 || !out_plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    *out_plan = NULL;

    /* Phase 1: Validate all package names and check registry */
    MultiPackageValidation *validation = arena_malloc(sizeof(MultiPackageValidation));
    if (!validation) {
        return SOLVER_INVALID_PACKAGE;
    }
    validation->results = arena_calloc(count, sizeof(PackageValidationResult));
    if (!validation->results) {
        arena_free(validation);
        return SOLVER_INVALID_PACKAGE;
    }
    validation->count = count;
    validation->valid_count = 0;
    validation->invalid_count = 0;

    for (int i = 0; i < count; i++) {
        PackageValidationResult *r = &validation->results[i];

        /* Already parsed - just copy */
        r->author = packages[i].author;
        r->name = packages[i].name;
        r->valid_name = true;

        /* Check registry */
        if (!package_exists_in_registry_internal(state, packages[i].author, packages[i].name)) {
            r->exists = false;
            r->error_msg = "Package not found in registry";
            validation->invalid_count++;
        } else {
            r->exists = true;
            r->error_msg = NULL;
            validation->valid_count++;
        }
    }

    /* Return validation results if requested */
    if (out_validation) {
        *out_validation = validation;
    }

    /* Phase 2: Fail if any packages are invalid */
    if (validation->invalid_count > 0) {
        if (!out_validation) {
            multi_package_validation_free(validation);
        }
        return SOLVER_INVALID_PACKAGE;
    }

    /* Phase 3: Solve each package, accumulating into combined plan */
    InstallPlan *combined_plan = install_plan_create();
    if (!combined_plan) {
        if (!out_validation) {
            multi_package_validation_free(validation);
        }
        return SOLVER_INVALID_PACKAGE;
    }

    for (int i = 0; i < count; i++) {
        const PackageVersionSpec *spec = &packages[i];
        InstallPlan *single_plan = NULL;

        SolverResult result = solver_add_package(
            state, elm_json,
            spec->author, spec->name,
            spec->version,  /* Pass target version from spec */
            is_test,
            false,  /* major_upgrade - not supported for multi */
            upgrade_all,
            &single_plan
        );

        if (result != SOLVER_OK) {
            /* Store which package caused the conflict */
            install_plan_free(combined_plan);
            if (single_plan) install_plan_free(single_plan);
            if (!out_validation) {
                multi_package_validation_free(validation);
            }
            return result;
        }

        /* Merge single_plan into combined_plan (deduplicating) */
        if (single_plan) {
            install_plan_merge(combined_plan, single_plan);
            install_plan_free(single_plan);
        }
    }

    *out_plan = combined_plan;

    /* Free validation if caller doesn't want it */
    if (!out_validation) {
        multi_package_validation_free(validation);
    }

    return SOLVER_OK;
}
