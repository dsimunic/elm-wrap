/**
 * init_v1.c - V1 Protocol Implementation for elm init
 *
 * This module handles dependency resolution for `elm init` using the V1 registry.
 */

#include "init_v1.h"
#include "../../install_env.h"
#include "../../elm_json.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../pgsolver/pg_core.h"
#include "../../pgsolver/pg_elm.h"
#include <stdio.h>
#include <string.h>

static const char *pg_name_resolver_v1(void *ctx, PgPackageId pkg) {
    PgElmContext *pg_ctx = (PgElmContext *)ctx;
    if (!pg_ctx || pkg < 0 || pkg >= pg_ctx->package_count) {
        return "?";
    }
    if (pkg == pg_elm_root_package_id()) {
        return "root";
    }
    // Return "author/name" format
    static char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s/%s", pg_ctx->authors[pkg], pg_ctx->names[pkg]);
    return buffer;
}

bool solve_init_dependencies_v1(InstallEnv *env, PackageMap **direct_deps, PackageMap **indirect_deps) {
    // Create PubGrub solver context for V1
    PgElmContext *pg_ctx = pg_elm_context_new(env, true);
    if (!pg_ctx) {
        log_error("Failed to create PubGrub solver context (V1)");
        return false;
    }

    // Add the three required packages as root dependencies with "any" version constraint
    const char *required_packages[][2] = {
        {"elm", "browser"},
        {"elm", "core"},
        {"elm", "html"}
    };

    for (int i = 0; i < 3; i++) {
        const char *author = required_packages[i][0];
        const char *name = required_packages[i][1];

        PgPackageId pkg_id = pg_elm_intern_package(pg_ctx, author, name);
        if (pkg_id < 0) {
            log_error("Failed to intern package %s/%s", author, name);
            pg_elm_context_free(pg_ctx);
            return false;
        }

        // Use "any" version constraint to get the latest
        PgVersionRange range = pg_range_any();
        if (!pg_elm_add_root_dependency(pg_ctx, pkg_id, range)) {
            log_error("Failed to add root dependency for %s/%s", author, name);
            pg_elm_context_free(pg_ctx);
            return false;
        }
    }

    // Create dependency provider
    PgDependencyProvider provider = pg_elm_make_provider(pg_ctx);
    PgPackageId root_pkg = pg_elm_root_package_id();

    PgVersion root_version;
    root_version.major = 1;
    root_version.minor = 0;
    root_version.patch = 0;

    // Create solver
    PgSolver *solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version);
    if (!solver) {
        log_error("Failed to create PubGrub solver");
        pg_elm_context_free(pg_ctx);
        return false;
    }

    // Run solver
    PgSolverStatus status = pg_solver_solve(solver);

    if (status != PG_SOLVER_OK) {
        log_error("Failed to solve dependencies");
        char error_msg[4096];
        if (pg_solver_explain_failure(solver, pg_name_resolver_v1, pg_ctx, error_msg, sizeof(error_msg))) {
            fprintf(stderr, "%s\n", error_msg);
        }
        pg_solver_free(solver);
        pg_elm_context_free(pg_ctx);
        return false;
    }

    // Create package maps for direct and indirect dependencies
    *direct_deps = package_map_create();
    *indirect_deps = package_map_create();

    if (!*direct_deps || !*indirect_deps) {
        log_error("Failed to create package maps");
        package_map_free(*direct_deps);
        package_map_free(*indirect_deps);
        pg_solver_free(solver);
        pg_elm_context_free(pg_ctx);
        return false;
    }

    // Add the three required packages to direct dependencies
    for (int i = 0; i < 3; i++) {
        const char *author = required_packages[i][0];
        const char *name = required_packages[i][1];

        PgPackageId pkg_id = pg_elm_intern_package(pg_ctx, author, name);
        PgVersion version;
        if (!pg_solver_get_selected_version(solver, pkg_id, &version)) {
            log_error("Failed to get version for %s/%s", author, name);
            package_map_free(*direct_deps);
            package_map_free(*indirect_deps);
            pg_solver_free(solver);
            pg_elm_context_free(pg_ctx);
            return false;
        }

        char version_str[32];
        snprintf(version_str, sizeof(version_str), "%d.%d.%d", version.major, version.minor, version.patch);

        if (!package_map_add(*direct_deps, author, name, version_str)) {
            log_error("Failed to add %s/%s to direct dependencies", author, name);
            package_map_free(*direct_deps);
            package_map_free(*indirect_deps);
            pg_solver_free(solver);
            pg_elm_context_free(pg_ctx);
            return false;
        }
    }

    // Get all other resolved packages and add them to indirect dependencies
    for (int pkg_id = 1; pkg_id < pg_ctx->package_count; pkg_id++) {
        // Skip the three direct dependencies
        bool is_direct = false;
        for (int i = 0; i < 3; i++) {
            const char *author = required_packages[i][0];
            const char *name = required_packages[i][1];
            PgPackageId direct_pkg_id = pg_elm_intern_package(pg_ctx, author, name);
            if (pkg_id == direct_pkg_id) {
                is_direct = true;
                break;
            }
        }
        if (is_direct) {
            continue;
        }

        PgVersion version;
        if (!pg_solver_get_selected_version(solver, pkg_id, &version)) {
            // No decision for this package, skip it
            continue;
        }

        const char *author = pg_ctx->authors[pkg_id];
        const char *name = pg_ctx->names[pkg_id];

        char version_str[32];
        snprintf(version_str, sizeof(version_str), "%d.%d.%d", version.major, version.minor, version.patch);

        if (!package_map_add(*indirect_deps, author, name, version_str)) {
            log_error("Failed to add %s/%s to indirect dependencies", author, name);
            // Continue anyway
        }
    }

    pg_solver_free(solver);
    pg_elm_context_free(pg_ctx);
    return true;
}
