#include "solver.h"
#include "../../install_env.h"
#include "../../pgsolver/pg_core.h"
#include "../../pgsolver/pg_elm.h"
#include "../../pgsolver/solver_common.h"
#include "../../cache.h"
#include "../../constants.h"
#include "../../log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Add a root dependency with an exact version constraint */
static bool solver_add_exact_root_dependency(
    PgElmContext *ctx,
    const char *author,
    const char *name,
    const char *version,
    const char *context_label
) {
    if (!ctx || !author || !name || !version) {
        return false;
    }

    PgPackageId pkg_id = pg_elm_intern_package(ctx, author, name);
    if (pkg_id < 0) {
        log_error("Failed to intern package %s/%s for %s",
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    PgVersion v;
    if (!pg_version_parse(version, &v)) {
        log_error("Invalid version '%s' for %s/%s (%s)",
                version,
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    /* Exact version constraint */
    PgVersionRange range = pg_range_exact(v);
    if (!pg_elm_add_root_dependency(ctx, pkg_id, range)) {
        log_error("Failed to add exact root dependency for %s/%s",
                author,
                name);
        return false;
    }
    return true;
}

static bool solver_add_upgradable_root_dependency(
    PgElmContext *ctx,
    const char *author,
    const char *name,
    const char *version,
    const char *context_label
) {
    if (!ctx || !author || !name || !version) {
        return false;
    }

    PgPackageId pkg_id = pg_elm_intern_package(ctx, author, name);
    if (pkg_id < 0) {
        log_error("Failed to intern package %s/%s for %s",
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    PgVersion v;
    if (!pg_version_parse(version, &v)) {
        log_error("Invalid version '%s' for %s/%s (%s)",
                version,
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    /* Allow upgrades within the same major version */
    PgVersionRange range = pg_range_until_next_major(v);
    if (!pg_elm_add_root_dependency(ctx, pkg_id, range)) {
        log_error("Failed to add upgradable root dependency for %s/%s",
                author,
                name);
        return false;
    }
    return true;
}

/* Add dependencies with exact version constraints */
static bool solver_add_exact_map_dependencies(
    PgElmContext *ctx,
    PackageMap *map,
    const char *label
) {
    if (!map) {
        return true;
    }
    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!solver_add_exact_root_dependency(
                ctx,
                pkg->author,
                pkg->name,
                pkg->version,
                label)) {
            return false;
        }
    }
    return true;
}

/* Add dependencies allowing upgrades within major version */
static bool solver_add_upgradable_map_dependencies(
    PgElmContext *ctx,
    PackageMap *map,
    const char *label
) {
    if (!map) {
        return true;
    }
    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!solver_add_upgradable_root_dependency(
                ctx,
                pkg->author,
                pkg->name,
                pkg->version,
                label)) {
            return false;
        }
    }
    return true;
}

static bool solver_add_constraint_root_dependency(
    PgElmContext *ctx,
    const char *author,
    const char *name,
    const char *constraint,
    const char *context_label
) {
    if (!ctx || !author || !name || !constraint) {
        return false;
    }

    PgPackageId pkg_id = pg_elm_intern_package(ctx, author, name);
    if (pkg_id < 0) {
        log_error("Failed to intern package %s/%s for %s",
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    PgVersionRange range;
    if (!pg_elm_parse_constraint(constraint, &range)) {
        log_error("Invalid constraint '%s' for %s/%s (%s)",
                constraint,
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    if (!pg_elm_add_root_dependency(ctx, pkg_id, range)) {
        log_error("Failed to add constraint dependency for %s/%s",
                author,
                name);
        return false;
    }

    return true;
}

static bool solver_add_constraint_map_dependencies(
    PgElmContext *ctx,
    PackageMap *map,
    const char *label
) {
    if (!map) {
        return true;
    }

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!solver_add_constraint_root_dependency(
                ctx,
                pkg->author,
                pkg->name,
                pkg->version,
                label)) {
            return false;
        }
    }
    return true;
}

/* Build root dependencies using exact version constraints (strategy 0) */
static bool build_roots_strategy_exact_app(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json,
    bool include_prod,
    bool include_test
) {
    bool ok = true;

    /* All dependencies get exact ranges */
    if (include_prod) {
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_direct,
            "dependencies_direct"
        );
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_indirect,
            "dependencies_indirect"
        );
    }
    if (include_test) {
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* Build root dependencies with exact direct, upgradable indirect (strategy 1) */
static bool build_roots_strategy_exact_direct_app(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json,
    bool include_prod,
    bool include_test
) {
    bool ok = true;

    /* Direct dependencies get exact ranges, indirect get upgradable */
    if (include_prod) {
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_direct,
            "dependencies_direct"
        );
        ok = ok && solver_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_indirect,
            "dependencies_indirect"
        );
    }
    /* Test dependencies stay exact to avoid unnecessary test framework upgrades */
    if (include_test) {
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* Build root dependencies allowing upgrades within major version (strategy 2) */
static bool build_roots_strategy_upgradable_app(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json,
    bool include_prod,
    bool include_test
) {
    bool ok = true;

    /* All dependencies get upgradable ranges */
    if (include_prod) {
        ok = ok && solver_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_direct,
            "dependencies_direct"
        );
        ok = ok && solver_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_indirect,
            "dependencies_indirect"
        );
    }
    if (include_test) {
        ok = ok && solver_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* Build root dependencies with cross-major upgrade for target package (strategy 3) */
static bool build_roots_strategy_cross_major_for_target(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json,
    const char *target_author,
    const char *target_name,
    bool include_test
) {
    (void)target_author; /* Unused - target already added before this function */
    (void)target_name;

    /* For cross-major upgrades, use MINIMAL root constraints.
     * The target package was already added with `any` range before this function.
     * For other packages:
     * - Direct dependencies: DON'T add as roots, let solver pick compatible versions
     * - Test dependencies: Keep exact to avoid unnecessary test changes
     *
     * This gives the solver maximum flexibility to find versions that work
     * together with the new major version of the target package.
     */

    log_debug("Cross-major strategy: skipping direct/indirect dependencies, only constraining tests");

    /* Add test dependencies with exact constraints to avoid unnecessary changes */
    bool ok = true;
    if (include_test) {
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* V1 Protocol: Run solver with a specific strategy */
SolverResult run_with_strategy_v1(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    SolverStrategy strategy,
    PackageMap *current_packages,
    InstallPlan **out_plan
) {
    PgElmContext *pg_ctx = pg_elm_context_new(state->install_env, state->online);
    if (!pg_ctx) {
        log_error("Failed to initialize PubGrub Elm context");
        return SOLVER_NETWORK_ERROR;
    }

    PgDependencyProvider provider = pg_elm_make_provider(pg_ctx);
    PgPackageId root_pkg = pg_elm_root_package_id();

    PgVersion root_version;
    root_version.major = 1;
    root_version.minor = 0;
    root_version.patch = 0;

    PgSolver *pg_solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version);
    if (!pg_solver) {
        pg_elm_context_free(pg_ctx);
        log_error("Failed to create PubGrub solver");
        return SOLVER_NETWORK_ERROR;
    }

    /* Build root dependencies based on strategy */
    bool root_ok = true;
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        bool include_prod = !is_test_dependency;
        bool include_test = is_test_dependency;

        /* For STRATEGY_CROSS_MAJOR_FOR_TARGET, add the target package FIRST
         * with an unconstrained range, so it has priority before other packages
         * add their transitive constraints. */
        if (strategy == STRATEGY_CROSS_MAJOR_FOR_TARGET) {
            PgPackageId target_pkg_id = pg_elm_intern_package(pg_ctx, author, name);
            if (target_pkg_id < 0) {
                pg_elm_context_free(pg_ctx);
                pg_solver_free(pg_solver);
                return SOLVER_INVALID_PACKAGE;
            }
            PgVersionRange target_range = pg_range_any();
            if (!pg_elm_add_root_dependency(pg_ctx, target_pkg_id, target_range)) {
                log_error("Failed to add target package %s/%s as root with any range", author, name);
                pg_elm_context_free(pg_ctx);
                pg_solver_free(pg_solver);
                return SOLVER_NO_SOLUTION;
            }
            log_debug("Added target package %s/%s as root with unconstrained range (ID=%d)",
                     author, name, target_pkg_id);
        }

        switch (strategy) {
            case STRATEGY_EXACT_ALL:
                log_debug("Trying strategy: exact versions for all dependencies");
                root_ok = build_roots_strategy_exact_app(pg_ctx, elm_json, include_prod, include_test);
                break;
            case STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT:
                log_debug("Trying strategy: exact direct, upgradable indirect dependencies");
                root_ok = build_roots_strategy_exact_direct_app(pg_ctx, elm_json, include_prod, include_test);
                break;
            case STRATEGY_UPGRADABLE_WITHIN_MAJOR:
                log_debug("Trying strategy: upgradable within major version");
                root_ok = build_roots_strategy_upgradable_app(pg_ctx, elm_json, include_prod, include_test);
                break;
            case STRATEGY_CROSS_MAJOR_FOR_TARGET:
                log_debug("Trying strategy: cross-major upgrade for %s/%s", author, name);
                root_ok = build_roots_strategy_cross_major_for_target(pg_ctx, elm_json, author, name, include_test);
                break;
        }
    } else {
        /* Packages use constraints from elm.json */
        bool include_prod = !is_test_dependency;
        bool include_test = is_test_dependency;

        if (include_prod) {
            root_ok = solver_add_constraint_map_dependencies(
                pg_ctx,
                elm_json->package_dependencies,
                "package_dependencies"
            );
        }
        if (root_ok && include_test) {
            root_ok = solver_add_constraint_map_dependencies(
                pg_ctx,
                elm_json->package_test_dependencies,
                "package_test_dependencies"
            );
        }
    }

    if (!root_ok) {
        log_error("Failed to register existing dependencies");
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        return SOLVER_NO_SOLUTION;
    }

    /* Add the requested package as a root dependency (if not already added).
     * For STRATEGY_CROSS_MAJOR_FOR_TARGET, we already added it above.
     */
    if (strategy != STRATEGY_CROSS_MAJOR_FOR_TARGET) {
        PgPackageId new_pkg_id = pg_elm_intern_package(pg_ctx, author, name);
        if (new_pkg_id < 0) {
            pg_solver_free(pg_solver);
            pg_elm_context_free(pg_ctx);
            return SOLVER_INVALID_PACKAGE;
        }

        PgVersionRange new_pkg_range = pg_range_any();
        if (!pg_elm_add_root_dependency(pg_ctx, new_pkg_id, new_pkg_range)) {
            log_error("Conflicting constraints for requested package %s/%s", author, name);
            pg_solver_free(pg_solver);
            pg_elm_context_free(pg_ctx);
            return SOLVER_NO_SOLUTION;
        }
    }

    /* Get the package ID for the requested package */
    PgPackageId new_pkg_id = pg_elm_intern_package(pg_ctx, author, name);
    if (new_pkg_id < 0) {
        log_error("Failed to intern package %s/%s", author, name);
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Run the PubGrub-style solver */
    PgSolverStatus pg_status = pg_solver_solve(pg_solver);
    if (pg_status != PG_SOLVER_OK) {
        log_debug("Strategy failed to find solution");
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        return SOLVER_NO_SOLUTION;
    }

    log_debug("Requested package %s/%s has ID %d", author, name, new_pkg_id);

    /* Extract the chosen version for the requested package */
    PgVersion chosen;
    if (!pg_solver_get_selected_version(pg_solver, new_pkg_id, &chosen)) {
        log_error("No version selected for %s/%s", author, name);
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        return SOLVER_NO_SOLUTION;
    }

    char selected_version[MAX_VERSION_STRING_LENGTH];
    snprintf(selected_version, sizeof(selected_version),
             "%d.%d.%d",
             chosen.major,
             chosen.minor,
             chosen.patch);

    log_debug("Selected version: %s", selected_version);

    /* Create install plan */
    InstallPlan *plan = install_plan_create();
    if (!plan) {
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Collect all selected packages */
    for (int i = 1; i < pg_ctx->package_count; i++) {
        PgPackageId pkg_id = (PgPackageId)i;
        PgVersion selected_ver;
        if (pg_solver_get_selected_version(pg_solver, pkg_id, &selected_ver)) {
            char version_str[MAX_VERSION_STRING_LENGTH];
            snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                     selected_ver.major, selected_ver.minor, selected_ver.patch);

            const char *pkg_author = pg_ctx->authors[i];
            const char *pkg_name = pg_ctx->names[i];

            /* Find if it was already in current packages */
            Package *existing = package_map_find(current_packages, pkg_author, pkg_name);
            const char *old_ver = existing ? existing->version : NULL;

            log_debug("Package[%d] %s/%s: old=%s new=%s", i, pkg_author, pkg_name,
                      old_ver ? old_ver : "NULL", version_str);

            /* Only add to plan if it's new or changed */
            if (!existing || strcmp(existing->version, version_str) != 0) {
                if (!install_plan_add_change(plan, pkg_author, pkg_name, old_ver, version_str)) {
                    install_plan_free(plan);
                    pg_solver_free(pg_solver);
                    pg_elm_context_free(pg_ctx);
                    return SOLVER_INVALID_PACKAGE;
                }
            }
        }
    }

    /* Ensure the requested package is downloaded */
    if (selected_version[0] && !cache_package_exists(state->cache, author, name, selected_version)) {
        log_debug("Package not in cache, downloading");
        if (state->install_env) {
            if (!cache_download_package_with_env(state->install_env, author, name, selected_version)) {
                install_plan_free(plan);
                pg_solver_free(pg_solver);
                pg_elm_context_free(pg_ctx);
                return SOLVER_NETWORK_ERROR;
            }
        } else {
            log_error("Cannot download package without InstallEnv");
            install_plan_free(plan);
            pg_solver_free(pg_solver);
            pg_elm_context_free(pg_ctx);
            return SOLVER_INVALID_PACKAGE;
        }
    } else {
        log_debug("Package found in cache");
    }

    pg_solver_free(pg_solver);
    pg_elm_context_free(pg_ctx);

    *out_plan = plan;
    log_debug("Plan created with %d changes", plan->count);
    return SOLVER_OK;
}

/* V1 Protocol: Upgrade all packages */
SolverResult solver_upgrade_all_v1(
    SolverState *state,
    const ElmJson *elm_json,
    bool major_upgrade,
    InstallPlan **out_plan
) {
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

    /* Build Elm-specific context and PubGrub provider */
    PgElmContext *pg_ctx = pg_elm_context_new(state->install_env, state->online);
    if (!pg_ctx) {
        log_error("Failed to initialize PubGrub Elm context");
        package_map_free(current_packages);
        return SOLVER_NETWORK_ERROR;
    }

    PgDependencyProvider provider = pg_elm_make_provider(pg_ctx);
    PgPackageId root_pkg = pg_elm_root_package_id();

    PgVersion root_version;
    root_version.major = 1;
    root_version.minor = 0;
    root_version.patch = 0;

    PgSolver *pg_solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version);
    if (!pg_solver) {
        pg_elm_context_free(pg_ctx);
        package_map_free(current_packages);
        log_error("Failed to create PubGrub solver");
        return SOLVER_NETWORK_ERROR;
    }

    /* Build root dependencies - allow upgrades for all packages */
    bool root_ok = true;
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        if (major_upgrade) {
            /* For major upgrades, allow any version */
            log_debug("Allowing major upgrades for all packages");
            /* Don't add any root constraints - let solver pick latest versions.
             * We rely on the registry/provider to expose only package versions that
             * are compatible with the current Elm compiler version (via ELM_HOME
             * being versioned per compiler), so the solver itself only ever sees
             * compatible packages here.
             */
            root_ok = true;
        } else {
            /* For minor upgrades, use upgradable within major strategy */
            log_debug("Using upgradable within major version strategy");
            root_ok = build_roots_strategy_upgradable_app(pg_ctx, elm_json, true, true);
        }
    } else {
        /* Packages use constraints from elm.json */
        root_ok = solver_add_constraint_map_dependencies(
            pg_ctx,
            elm_json->package_dependencies,
            "package_dependencies"
        );
        root_ok = root_ok && solver_add_constraint_map_dependencies(
            pg_ctx,
            elm_json->package_test_dependencies,
            "package_test_dependencies"
        );
    }

    if (!root_ok) {
        log_error("Failed to register existing dependencies");
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        package_map_free(current_packages);
        return SOLVER_NO_SOLUTION;
    }

    /* Run the PubGrub-style solver */
    PgSolverStatus pg_status = pg_solver_solve(pg_solver);
    if (pg_status != PG_SOLVER_OK) {
        log_debug("Upgrade failed to find solution");
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        package_map_free(current_packages);
        return SOLVER_NO_SOLUTION;
    }

    /* Create install plan */
    InstallPlan *plan = install_plan_create();
    if (!plan) {
        pg_solver_free(pg_solver);
        pg_elm_context_free(pg_ctx);
        package_map_free(current_packages);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Collect all selected packages */
    for (int i = 1; i < pg_ctx->package_count; i++) {
        PgPackageId pkg_id = (PgPackageId)i;
        PgVersion selected_ver;
        if (pg_solver_get_selected_version(pg_solver, pkg_id, &selected_ver)) {
            char version_str[MAX_VERSION_STRING_LENGTH];
            snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                     selected_ver.major, selected_ver.minor, selected_ver.patch);

            const char *pkg_author = pg_ctx->authors[i];
            const char *pkg_name = pg_ctx->names[i];

            /* Find if it was already in current packages */
            Package *existing = package_map_find(current_packages, pkg_author, pkg_name);
            const char *old_ver = existing ? existing->version : NULL;

            log_debug("Package[%d] %s/%s: old=%s new=%s", i, pkg_author, pkg_name,
                      old_ver ? old_ver : "NULL", version_str);

            /* Only add to plan if it changed */
            if (existing && strcmp(existing->version, version_str) != 0) {
                if (!install_plan_add_change(plan, pkg_author, pkg_name, old_ver, version_str)) {
                    install_plan_free(plan);
                    pg_solver_free(pg_solver);
                    pg_elm_context_free(pg_ctx);
                    package_map_free(current_packages);
                    return SOLVER_INVALID_PACKAGE;
                }
            }
        }
    }

    pg_solver_free(pg_solver);
    pg_elm_context_free(pg_ctx);
    package_map_free(current_packages);

    *out_plan = plan;
    log_debug("Upgrade plan created with %d changes", plan->count);
    return SOLVER_OK;
}
