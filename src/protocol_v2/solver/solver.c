#include "solver.h"
#include "../../install_env.h"
#include "../../pgsolver/pg_core.h"
#include "pg_elm_v2.h"
#include "v2_registry.h"
#include "../../pgsolver/solver_common.h"
#include "../../cache.h"
#include "../../log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Add a root dependency with an exact version constraint (V2) */
static bool solver_v2_add_exact_root_dependency(
    PgElmV2Context *ctx,
    const char *author,
    const char *name,
    const char *version,
    const char *context_label
) {
    if (!ctx || !author || !name || !version) {
        return false;
    }

    PgPackageId pkg_id = pg_elm_v2_intern_package(ctx, author, name);
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
    if (!pg_elm_v2_add_root_dependency(ctx, pkg_id, range)) {
        log_error("Failed to add exact root dependency for %s/%s",
                author,
                name);
        return false;
    }
    return true;
}

/* Add a root dependency allowing upgrades within major version (V2) */
static bool solver_v2_add_upgradable_root_dependency(
    PgElmV2Context *ctx,
    const char *author,
    const char *name,
    const char *version,
    const char *context_label
) {
    if (!ctx || !author || !name || !version) {
        return false;
    }

    PgPackageId pkg_id = pg_elm_v2_intern_package(ctx, author, name);
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
    if (!pg_elm_v2_add_root_dependency(ctx, pkg_id, range)) {
        log_error("Failed to add upgradable root dependency for %s/%s",
                author,
                name);
        return false;
    }
    return true;
}

/* Add dependencies with exact version constraints (V2) */
static bool solver_v2_add_exact_map_dependencies(
    PgElmV2Context *ctx,
    PackageMap *map,
    const char *label
) {
    if (!map) {
        return true;
    }
    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!solver_v2_add_exact_root_dependency(
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

/* Add dependencies allowing upgrades within major version (V2) */
static bool solver_v2_add_upgradable_map_dependencies(
    PgElmV2Context *ctx,
    PackageMap *map,
    const char *label
) {
    if (!map) {
        return true;
    }
    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!solver_v2_add_upgradable_root_dependency(
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

/* Add a root dependency with a constraint string (V2) */
static bool solver_v2_add_constraint_root_dependency(
    PgElmV2Context *ctx,
    const char *author,
    const char *name,
    const char *constraint,
    const char *context_label
) {
    if (!ctx || !author || !name || !constraint) {
        return false;
    }

    PgPackageId pkg_id = pg_elm_v2_intern_package(ctx, author, name);
    if (pkg_id < 0) {
        log_error("Failed to intern package %s/%s for %s",
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    PgVersionRange range;
    if (!pg_elm_v2_parse_constraint(constraint, &range)) {
        log_error("Invalid constraint '%s' for %s/%s (%s)",
                constraint,
                author,
                name,
                context_label ? context_label : "root");
        return false;
    }

    if (!pg_elm_v2_add_root_dependency(ctx, pkg_id, range)) {
        log_error("Failed to add constraint dependency for %s/%s",
                author,
                name);
        return false;
    }

    return true;
}

/* Add dependencies with constraint strings (V2) */
static bool solver_v2_add_constraint_map_dependencies(
    PgElmV2Context *ctx,
    PackageMap *map,
    const char *label
) {
    if (!map) {
        return true;
    }

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!solver_v2_add_constraint_root_dependency(
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

/* Build root dependencies using exact version constraints (V2, strategy 0) */
static bool build_roots_strategy_exact_app_v2(
    PgElmV2Context *pg_ctx,
    const ElmJson *elm_json,
    bool include_prod,
    bool include_test
) {
    bool ok = true;

    /* All dependencies get exact ranges */
    if (include_prod) {
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_direct,
            "dependencies_direct"
        );
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_indirect,
            "dependencies_indirect"
        );
    }
    if (include_test) {
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* Build root dependencies with exact direct, upgradable indirect (V2, strategy 1) */
static bool build_roots_strategy_exact_direct_app_v2(
    PgElmV2Context *pg_ctx,
    const ElmJson *elm_json,
    bool include_prod,
    bool include_test
) {
    bool ok = true;

    /* Direct dependencies get exact ranges, indirect get upgradable */
    if (include_prod) {
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_direct,
            "dependencies_direct"
        );
        ok = ok && solver_v2_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_indirect,
            "dependencies_indirect"
        );
    }
    /* Test dependencies stay exact to avoid unnecessary test framework upgrades */
    if (include_test) {
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* Build root dependencies allowing upgrades within major version (V2, strategy 2) */
static bool build_roots_strategy_upgradable_app_v2(
    PgElmV2Context *pg_ctx,
    const ElmJson *elm_json,
    bool include_prod,
    bool include_test
) {
    bool ok = true;

    /* All dependencies get upgradable ranges */
    if (include_prod) {
        ok = ok && solver_v2_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_direct,
            "dependencies_direct"
        );
        ok = ok && solver_v2_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_indirect,
            "dependencies_indirect"
        );
    }
    if (include_test) {
        ok = ok && solver_v2_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_v2_add_upgradable_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* Build root dependencies with cross-major upgrade for target package (V2, strategy 3) */
static bool build_roots_strategy_cross_major_for_target_v2(
    PgElmV2Context *pg_ctx,
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
     */

    log_trace("Cross-major strategy (V2): skipping direct/indirect dependencies, only constraining tests");

    bool ok = true;
    if (include_test) {
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_direct,
            "dependencies_test_direct"
        );
        ok = ok && solver_v2_add_exact_map_dependencies(
            pg_ctx,
            elm_json->dependencies_test_indirect,
            "dependencies_test_indirect"
        );
    }

    return ok;
}

/* V2 Protocol: Run solver with a specific strategy */
SolverResult run_with_strategy_v2(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    SolverStrategy strategy,
    PackageMap *current_packages,
    InstallPlan **out_plan
) {
    (void)is_test_dependency; /* May be used in future strategies */

    if (!state->install_env->v2_registry) {
        log_error("V2 mode but no V2 registry loaded");
        return SOLVER_NETWORK_ERROR;
    }

    PgElmV2Context *pg_ctx = pg_elm_v2_context_new(state->install_env->v2_registry);
    if (!pg_ctx) {
        log_error("Failed to initialize PubGrub V2 Elm context");
        return SOLVER_NETWORK_ERROR;
    }

    PgDependencyProvider provider = pg_elm_v2_make_provider(pg_ctx);
    PgPackageId root_pkg = pg_elm_v2_root_package_id();

    PgVersion root_version;
    root_version.major = 1;
    root_version.minor = 0;
    root_version.patch = 0;

    PgSolver *pg_solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version);
    if (!pg_solver) {
        pg_elm_v2_context_free(pg_ctx);
        log_error("Failed to create PubGrub solver (V2)");
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
            PgPackageId target_pkg_id = pg_elm_v2_intern_package(pg_ctx, author, name);
            if (target_pkg_id < 0) {
                pg_elm_v2_context_free(pg_ctx);
                pg_solver_free(pg_solver);
                return SOLVER_INVALID_PACKAGE;
            }
            PgVersionRange target_range = pg_range_any();
            if (!pg_elm_v2_add_root_dependency(pg_ctx, target_pkg_id, target_range)) {
                log_error("Failed to add target package %s/%s as root with any range (V2)", author, name);
                pg_elm_v2_context_free(pg_ctx);
                pg_solver_free(pg_solver);
                return SOLVER_NO_SOLUTION;
            }
            log_trace("Added target package %s/%s as root with unconstrained range (V2, ID=%d)",
                     author, name, target_pkg_id);
        }

        switch (strategy) {
            case STRATEGY_EXACT_ALL:
                log_trace("Trying strategy (V2): exact versions for all dependencies");
                root_ok = build_roots_strategy_exact_app_v2(pg_ctx, elm_json, include_prod, include_test);
                break;
            case STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT:
                log_trace("Trying strategy (V2): exact direct, upgradable indirect dependencies");
                root_ok = build_roots_strategy_exact_direct_app_v2(pg_ctx, elm_json, include_prod, include_test);
                break;
            case STRATEGY_UPGRADABLE_WITHIN_MAJOR:
                log_trace("Trying strategy (V2): upgradable within major version");
                root_ok = build_roots_strategy_upgradable_app_v2(pg_ctx, elm_json, include_prod, include_test);
                break;
            case STRATEGY_CROSS_MAJOR_FOR_TARGET:
                log_trace("Trying strategy (V2): cross-major upgrade for %s/%s", author, name);
                root_ok = build_roots_strategy_cross_major_for_target_v2(pg_ctx, elm_json, author, name, include_test);
                break;
        }
    } else {
        /* Packages use constraints from elm.json */
        bool include_prod = !is_test_dependency;
        bool include_test = is_test_dependency;

        if (include_prod) {
            root_ok = solver_v2_add_constraint_map_dependencies(
                pg_ctx,
                elm_json->package_dependencies,
                "package_dependencies"
            );
        }
        if (root_ok && include_test) {
            root_ok = solver_v2_add_constraint_map_dependencies(
                pg_ctx,
                elm_json->package_test_dependencies,
                "package_test_dependencies"
            );
        }
    }

    if (!root_ok) {
        log_error("Failed to register existing dependencies (V2)");
        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        return SOLVER_NO_SOLUTION;
    }

    /* Add the requested package as a root dependency (if not already added).
     * For STRATEGY_CROSS_MAJOR_FOR_TARGET, we already added it above.
     */
    if (strategy != STRATEGY_CROSS_MAJOR_FOR_TARGET) {
        PgPackageId new_pkg_id = pg_elm_v2_intern_package(pg_ctx, author, name);
        if (new_pkg_id < 0) {
            pg_solver_free(pg_solver);
            pg_elm_v2_context_free(pg_ctx);
            return SOLVER_INVALID_PACKAGE;
        }

        PgVersionRange new_pkg_range = pg_range_any();
        if (!pg_elm_v2_add_root_dependency(pg_ctx, new_pkg_id, new_pkg_range)) {
            log_error("Conflicting constraints for requested package %s/%s (V2)", author, name);
            pg_solver_free(pg_solver);
            pg_elm_v2_context_free(pg_ctx);
            return SOLVER_NO_SOLUTION;
        }
    }

    /* Get the package ID for the requested package */
    PgPackageId new_pkg_id = pg_elm_v2_intern_package(pg_ctx, author, name);
    if (new_pkg_id < 0) {
        log_error("Failed to intern package %s/%s (V2)", author, name);
        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Run the PubGrub-style solver */
    PgSolverStatus pg_status = pg_solver_solve(pg_solver);
    if (pg_status != PG_SOLVER_OK) {
        log_trace("Strategy failed to find solution (V2)");

        /* Generate a human-readable error explanation */
        char error_buffer[4096];
        PgExplainContext explain_ctx;
        explain_ctx.resolver_ctx = pg_ctx;
        explain_ctx.current_packages = current_packages;

        if (pg_solver_explain_failure(pg_solver,
                                      (PgPackageNameResolver)pg_elm_v2_get_package_name_with_ctx,
                                      &explain_ctx,
                                      error_buffer,
                                      sizeof(error_buffer))) {
            log_error("Solver conflict:\n%s", error_buffer);
        }

        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        return SOLVER_NO_SOLUTION;
    }

    /* Get and print solver statistics */
    PgSolverStats stats;
    pg_solver_get_stats(pg_solver, &stats);
    if (log_is_progress()) {
        fprintf(stderr, "\n");
        fprintf(stderr, "Solver Statistics:\n");
        fprintf(stderr, "  Strategy: ");
        switch (strategy) {
            case STRATEGY_EXACT_ALL:
                fprintf(stderr, "exact versions for all dependencies\n");
                break;
            case STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT:
                fprintf(stderr, "exact direct, upgradable indirect\n");
                break;
            case STRATEGY_UPGRADABLE_WITHIN_MAJOR:
                fprintf(stderr, "upgradable within major version\n");
                break;
            case STRATEGY_CROSS_MAJOR_FOR_TARGET:
                fprintf(stderr, "cross-major upgrade\n");
                break;
        }
        fprintf(stderr, "  Registry lookups:  %d (cache hits: %d, misses: %d)\n",
                stats.cache_hits + stats.cache_misses, stats.cache_hits, stats.cache_misses);
        fprintf(stderr, "  Decisions:         %d\n", stats.decisions);
        fprintf(stderr, "  Propagations:      %d\n", stats.propagations);
        fprintf(stderr, "  Conflicts:         %d\n", stats.conflicts);
        fprintf(stderr, "\n");
    }

    log_trace("Requested package %s/%s has ID %d (V2)", author, name, new_pkg_id);

    /* Extract the chosen version for the requested package */
    PgVersion chosen;
    if (!pg_solver_get_selected_version(pg_solver, new_pkg_id, &chosen)) {
        log_error("No version selected for %s/%s (V2)", author, name);
        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        return SOLVER_NO_SOLUTION;
    }

    char selected_version[32];
    snprintf(selected_version, sizeof(selected_version),
             "%d.%d.%d",
             chosen.major,
             chosen.minor,
             chosen.patch);

    log_trace("Selected version (V2): %s", selected_version);

    /* Create install plan */
    InstallPlan *plan = install_plan_create();
    if (!plan) {
        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Collect all selected packages */
    for (int i = 1; i < pg_ctx->package_count; i++) {
        PgPackageId pkg_id = (PgPackageId)i;
        PgVersion selected_ver;
        if (pg_solver_get_selected_version(pg_solver, pkg_id, &selected_ver)) {
            char version_str[32];
            snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                     selected_ver.major, selected_ver.minor, selected_ver.patch);

            const char *pkg_author = pg_ctx->authors[i];
            const char *pkg_name = pg_ctx->names[i];

            /* Find if it was already in current packages */
            Package *existing = package_map_find(current_packages, pkg_author, pkg_name);
            const char *old_ver = existing ? existing->version : NULL;

            log_trace("Package[%d] %s/%s: old=%s new=%s (V2)", i, pkg_author, pkg_name,
                      old_ver ? old_ver : "NULL", version_str);

            /* Only add to plan if it's new or changed */
            if (!existing || strcmp(existing->version, version_str) != 0) {
                if (!install_plan_add_change(plan, pkg_author, pkg_name, old_ver, version_str)) {
                    install_plan_free(plan);
                    pg_solver_free(pg_solver);
                    pg_elm_v2_context_free(pg_ctx);
                    return SOLVER_INVALID_PACKAGE;
                }
            }
        }
    }

    /* Ensure the requested package is downloaded.
     * Note: V2 download path may differ from V1, but we use the same
     * install_env interface which handles protocol differences internally.
     */
    if (selected_version[0] && !cache_package_exists(state->cache, author, name, selected_version)) {
        log_trace("Package not in cache, downloading (V2)");
        if (state->install_env) {
            if (!cache_download_package_with_env(state->install_env, author, name, selected_version)) {
                install_plan_free(plan);
                pg_solver_free(pg_solver);
                pg_elm_v2_context_free(pg_ctx);
                return SOLVER_NETWORK_ERROR;
            }
        } else {
            log_error("Cannot download package without InstallEnv (V2)");
            install_plan_free(plan);
            pg_solver_free(pg_solver);
            pg_elm_v2_context_free(pg_ctx);
            return SOLVER_INVALID_PACKAGE;
        }
    } else {
        log_trace("Package found in cache (V2)");
    }

    pg_solver_free(pg_solver);
    pg_elm_v2_context_free(pg_ctx);

    *out_plan = plan;
    log_trace("Plan created with %d changes (V2)", plan->count);
    return SOLVER_OK;
}

/* V2 Protocol: Upgrade all packages */
SolverResult solver_upgrade_all_v2(
    SolverState *state,
    const ElmJson *elm_json,
    bool major_upgrade,
    InstallPlan **out_plan
) {
    if (!state->install_env->v2_registry) {
        log_error("V2 mode but no V2 registry loaded");
        return SOLVER_NETWORK_ERROR;
    }

    /* Collect current packages */
    PackageMap *current_packages = collect_current_packages(elm_json);
    if (!current_packages) {
        return SOLVER_INVALID_PACKAGE;
    }

    PgElmV2Context *pg_ctx = pg_elm_v2_context_new(state->install_env->v2_registry);
    if (!pg_ctx) {
        log_error("Failed to initialize PubGrub V2 Elm context");
        package_map_free(current_packages);
        return SOLVER_NETWORK_ERROR;
    }

    PgDependencyProvider provider = pg_elm_v2_make_provider(pg_ctx);
    PgPackageId root_pkg = pg_elm_v2_root_package_id();

    PgVersion root_version;
    root_version.major = 1;
    root_version.minor = 0;
    root_version.patch = 0;

    PgSolver *pg_solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version);
    if (!pg_solver) {
        pg_elm_v2_context_free(pg_ctx);
        package_map_free(current_packages);
        log_error("Failed to create PubGrub solver (V2 upgrade)");
        return SOLVER_NETWORK_ERROR;
    }

    /* Build root dependencies - allow upgrades for all packages */
    bool root_ok = true;
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        if (major_upgrade) {
            /* For major upgrades, allow any version */
            log_trace("Allowing major upgrades for all packages (V2)");
            /* Don't add any root constraints - let solver pick latest versions.
             * V2 registry already contains only versions compatible with
             * the current compiler.
             */
            root_ok = true;
        } else {
            /* For minor upgrades, use upgradable within major strategy */
            log_trace("Using upgradable within major version strategy (V2)");
            root_ok = build_roots_strategy_upgradable_app_v2(pg_ctx, elm_json, true, true);
        }
    } else {
        /* Packages use constraints from elm.json */
        root_ok = solver_v2_add_constraint_map_dependencies(
            pg_ctx,
            elm_json->package_dependencies,
            "package_dependencies"
        );
        root_ok = root_ok && solver_v2_add_constraint_map_dependencies(
            pg_ctx,
            elm_json->package_test_dependencies,
            "package_test_dependencies"
        );
    }

    if (!root_ok) {
        log_error("Failed to register existing dependencies (V2 upgrade)");
        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        package_map_free(current_packages);
        return SOLVER_NO_SOLUTION;
    }

    /* Run the PubGrub-style solver */
    PgSolverStatus pg_status = pg_solver_solve(pg_solver);
    if (pg_status != PG_SOLVER_OK) {
        log_trace("Upgrade failed to find solution (V2)");

        /* Generate a human-readable error explanation */
        char error_buffer[4096];
        PgExplainContext explain_ctx;
        explain_ctx.resolver_ctx = pg_ctx;
        explain_ctx.current_packages = current_packages;

        if (pg_solver_explain_failure(pg_solver,
                                      (PgPackageNameResolver)pg_elm_v2_get_package_name_with_ctx,
                                      &explain_ctx,
                                      error_buffer,
                                      sizeof(error_buffer))) {
            log_error("Solver conflict:\n%s", error_buffer);
        }

        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        package_map_free(current_packages);
        return SOLVER_NO_SOLUTION;
    }

    /* Create install plan */
    InstallPlan *plan = install_plan_create();
    if (!plan) {
        pg_solver_free(pg_solver);
        pg_elm_v2_context_free(pg_ctx);
        package_map_free(current_packages);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Collect all selected packages */
    for (int i = 1; i < pg_ctx->package_count; i++) {
        PgPackageId pkg_id = (PgPackageId)i;
        PgVersion selected_ver;
        if (pg_solver_get_selected_version(pg_solver, pkg_id, &selected_ver)) {
            char version_str[32];
            snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                     selected_ver.major, selected_ver.minor, selected_ver.patch);

            const char *pkg_author = pg_ctx->authors[i];
            const char *pkg_name = pg_ctx->names[i];

            /* Find if it was already in current packages */
            Package *existing = package_map_find(current_packages, pkg_author, pkg_name);
            const char *old_ver = existing ? existing->version : NULL;

            log_trace("Package[%d] %s/%s: old=%s new=%s (V2)", i, pkg_author, pkg_name,
                      old_ver ? old_ver : "NULL", version_str);

            /* Only add to plan if it changed */
            if (existing && strcmp(existing->version, version_str) != 0) {
                if (!install_plan_add_change(plan, pkg_author, pkg_name, old_ver, version_str)) {
                    install_plan_free(plan);
                    pg_solver_free(pg_solver);
                    pg_elm_v2_context_free(pg_ctx);
                    package_map_free(current_packages);
                    return SOLVER_INVALID_PACKAGE;
                }
            }
        }
    }

    pg_solver_free(pg_solver);
    pg_elm_v2_context_free(pg_ctx);
    package_map_free(current_packages);

    *out_plan = plan;
    log_trace("Upgrade plan created with %d changes (V2)", plan->count);
    return SOLVER_OK;
}
