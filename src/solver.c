#include "solver.h"
#include "install_env.h"
#include "pgsolver/pg_core.h"
#include "pgsolver/pg_elm.h"
#include "alloc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* InstallPlan operations */
InstallPlan* install_plan_create(void) {
    InstallPlan *plan = (InstallPlan *)arena_malloc(sizeof(InstallPlan));
    if (!plan) return NULL;
    plan->capacity = 8;
    plan->count = 0;
    plan->changes = (PackageChange *)arena_calloc(plan->capacity, sizeof(PackageChange));
    if (!plan->changes) {
        arena_free(plan);
        return NULL;
    }
    return plan;
}

void install_plan_free(InstallPlan *plan) {
    if (!plan) return;
    for (int i = 0; i < plan->count; i++) {
        arena_free(plan->changes[i].author);
        arena_free(plan->changes[i].name);
        arena_free(plan->changes[i].old_version);
        arena_free(plan->changes[i].new_version);
    }
    arena_free(plan->changes);
    arena_free(plan);
}

bool install_plan_add_change(InstallPlan *plan, const char *author, const char *name, const char *old_version, const char *new_version) {
    if (!plan || !author || !name) return false;
    /* Either old_version or new_version must be set (or both for upgrades) */
    if (!old_version && !new_version) return false;
    
    if (plan->count >= plan->capacity) {
        int new_capacity = plan->capacity * 2;
        PackageChange *new_changes = (PackageChange *)arena_realloc(plan->changes, new_capacity * sizeof(PackageChange));
        if (!new_changes) return false;
        plan->changes = new_changes;
        plan->capacity = new_capacity;
    }
    
    PackageChange *change = &plan->changes[plan->count];
    change->author = arena_strdup(author);
    change->name = arena_strdup(name);
    change->old_version = old_version ? arena_strdup(old_version) : NULL;
    change->new_version = new_version ? arena_strdup(new_version) : NULL;
    
    if (!change->author || !change->name || 
        (old_version && !change->old_version) ||
        (new_version && !change->new_version)) {
        arena_free(change->author);
        arena_free(change->name);
        arena_free(change->old_version);
        arena_free(change->new_version);
        return false;
    }
    
    plan->count++;
    return true;
}

/* Helper to collect all current packages */
static PackageMap* collect_current_packages(const ElmJson *elm_json) {
    PackageMap *current = package_map_create();
    if (!current) return NULL;
    
    PackageMap *maps[4] = {NULL};
    int map_count = 0;
    
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        maps[0] = elm_json->dependencies_direct;
        maps[1] = elm_json->dependencies_indirect;
        maps[2] = elm_json->dependencies_test_direct;
        maps[3] = elm_json->dependencies_test_indirect;
        map_count = 4;
    } else {
        maps[0] = elm_json->package_dependencies;
        maps[1] = elm_json->package_test_dependencies;
        map_count = 2;
    }
    
    for (int i = 0; i < map_count; i++) {
        if (maps[i]) {
            for (int j = 0; j < maps[i]->count; j++) {
                Package *pkg = &maps[i]->packages[j];
                package_map_add(current, pkg->author, pkg->name, pkg->version);
            }
        }
    }
    
    return current;
}

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

    // Exact version constraint
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

    // Allow upgrades within the same major version
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

/* Solver state operations */
SolverState* solver_init(struct InstallEnv *install_env, bool online) {
    SolverState *state = arena_malloc(sizeof(SolverState));
    if (!state) return NULL;

    state->install_env = install_env;
    state->cache = install_env ? install_env->cache : NULL;
    state->online = online;

    return state;
}

void solver_free(SolverState *state) {
    if (!state) return;
    arena_free(state);
}

/* Constraint operations */
Constraint* constraint_create_exact(const char *version) {
    Constraint *c = arena_malloc(sizeof(Constraint));
    if (!c) return NULL;
    
    c->type = CONSTRAINT_EXACT;
    c->exact_version = arena_strdup(version);
    
    return c;
}

Constraint* constraint_create_until_next_minor(const char *version) {
    Constraint *c = arena_malloc(sizeof(Constraint));
    if (!c) return NULL;
    
    c->type = CONSTRAINT_UNTIL_NEXT_MINOR;
    c->exact_version = arena_strdup(version);
    
    return c;
}

Constraint* constraint_create_until_next_major(const char *version) {
    Constraint *c = arena_malloc(sizeof(Constraint));
    if (!c) return NULL;
    
    c->type = CONSTRAINT_UNTIL_NEXT_MAJOR;
    c->exact_version = arena_strdup(version);
    
    return c;
}

Constraint* constraint_create_any(void) {
    Constraint *c = arena_malloc(sizeof(Constraint));
    if (!c) return NULL;
    
    c->type = CONSTRAINT_ANY;
    c->exact_version = NULL;
    
    return c;
}

void constraint_free(Constraint *constraint) {
    if (!constraint) return;
    
    if (constraint->exact_version) {
        arena_free(constraint->exact_version);
    }
    
    arena_free(constraint);
}

/* Version comparison helper */
static void parse_version(const char *version, int *major, int *minor, int *patch) {
    *major = 0;
    *minor = 0;
    *patch = 0;
    
    if (version) {
        sscanf(version, "%d.%d.%d", major, minor, patch);
    }
}

int version_compare(const char *v1, const char *v2) {
    int major1, minor1, patch1;
    int major2, minor2, patch2;
    
    parse_version(v1, &major1, &minor1, &patch1);
    parse_version(v2, &major2, &minor2, &patch2);
    
    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

bool version_satisfies(const char *version, Constraint *constraint) {
    if (!version || !constraint) return false;
    
    switch (constraint->type) {
        case CONSTRAINT_EXACT:
            return strcmp(version, constraint->exact_version) == 0;
            
        case CONSTRAINT_UNTIL_NEXT_MINOR: {
            int major, minor, patch;
            int req_major, req_minor, req_patch;
            
            parse_version(version, &major, &minor, &patch);
            parse_version(constraint->exact_version, &req_major, &req_minor, &req_patch);
            
            // Must have same major and minor, but patch can be higher
            return (major == req_major && 
                    minor == req_minor && 
                    patch >= req_patch);
        }
        
        case CONSTRAINT_UNTIL_NEXT_MAJOR: {
            int major, minor, patch;
            int req_major, req_minor, req_patch;
            
            parse_version(version, &major, &minor, &patch);
            parse_version(constraint->exact_version, &req_major, &req_minor, &req_patch);
            
            // Must have same major, but minor and patch can be higher
            if (major != req_major) return false;
            if (minor < req_minor) return false;
            if (minor == req_minor && patch < req_patch) return false;
            return true;
        }
        
        case CONSTRAINT_ANY:
            return true;
    }
    
    return false;
}

/* Registry operations (stubbed) */
char** solver_get_available_versions(SolverState *state, const char *author, const char *name, int *count) {
    (void)state;  // Unused for now
    
    fprintf(stderr, "[STUB] solver_get_available_versions: Would query registry for %s/%s\n", author, name);
    
    // TODO: Implement registry parsing
    // 1. Load registry.dat
    // 2. Find package entry
    // 3. Return list of versions (newest first)
    
    // For now, return dummy versions
    *count = 3;
    char **versions = arena_malloc(sizeof(char*) * (*count));
    versions[0] = arena_strdup("1.0.5");
    versions[1] = arena_strdup("1.0.4");
    versions[2] = arena_strdup("1.0.3");
    
    return versions;
}

void solver_free_versions(char **versions, int count) {
    if (!versions) return;
    
    for (int i = 0; i < count; i++) {
        arena_free(versions[i]);
    }
    arena_free(versions);
}

/* Solver strategies for package installations */
typedef enum {
    STRATEGY_EXACT_ALL,                        /* Pin all existing dependencies to exact versions */
    STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT, /* Pin direct deps, allow indirect to upgrade */
    STRATEGY_UPGRADABLE_WITHIN_MAJOR,          /* Allow upgrades within major version */
    STRATEGY_CROSS_MAJOR_FOR_TARGET            /* Allow cross-major upgrade for target package */
} SolverStrategy;

/* Build root dependencies using exact version constraints (strategy 0) */
static bool build_roots_strategy_exact_app(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json
) {
    bool ok = true;

    /* All dependencies get exact ranges */
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

    return ok;
}

/* Build root dependencies with exact direct, upgradable indirect (strategy 1) */
static bool build_roots_strategy_exact_direct_app(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json
) {
    bool ok = true;

    /* Direct dependencies get exact ranges, indirect get upgradable */
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
    /* Test dependencies stay exact to avoid unnecessary test framework upgrades */
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

    return ok;
}

/* Build root dependencies allowing upgrades within major version (strategy 2) */
static bool build_roots_strategy_upgradable_app(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json
) {
    bool ok = true;

    /* All dependencies get upgradable ranges */
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

    return ok;
}

/* Build root dependencies with cross-major upgrade for target package (strategy 3) */
static bool build_roots_strategy_cross_major_for_target(
    PgElmContext *pg_ctx,
    const ElmJson *elm_json,
    const char *target_author,
    const char *target_name
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

    return ok;
}

/* Helper function to run solver with a specific strategy */
static SolverResult run_with_strategy(
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

    /* Build Elm-specific context and PubGrub provider */
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
                root_ok = build_roots_strategy_exact_app(pg_ctx, elm_json);
                break;
            case STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT:
                log_debug("Trying strategy: exact direct, upgradable indirect dependencies");
                root_ok = build_roots_strategy_exact_direct_app(pg_ctx, elm_json);
                break;
            case STRATEGY_UPGRADABLE_WITHIN_MAJOR:
                log_debug("Trying strategy: upgradable within major version");
                root_ok = build_roots_strategy_upgradable_app(pg_ctx, elm_json);
                break;
            case STRATEGY_CROSS_MAJOR_FOR_TARGET:
                log_debug("Trying strategy: cross-major upgrade for %s/%s", author, name);
                root_ok = build_roots_strategy_cross_major_for_target(pg_ctx, elm_json, author, name);
                break;
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

    char selected_version[32];
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
            char version_str[32];
            snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                     selected_ver.major, selected_ver.minor, selected_ver.patch);

            const char *pkg_author = pg_ctx->authors[i];
            const char *pkg_name = pg_ctx->names[i];

            // Find if it was already in current packages
            Package *existing = package_map_find(current_packages, pkg_author, pkg_name);
            const char *old_ver = existing ? existing->version : NULL;

            log_debug("Package[%d] %s/%s: old=%s new=%s", i, pkg_author, pkg_name, 
                      old_ver ? old_ver : "NULL", version_str);

            // Only add to plan if it's new or changed
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

/* Main solver function */
SolverResult solver_add_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    bool major_upgrade,
    InstallPlan **out_plan
) {
    if (!state || !elm_json || !author || !name || !out_plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    *out_plan = NULL;

    log_debug("Adding package: %s/%s%s%s",
            author, name, is_test_dependency ? " (test dependency)" : "",
            major_upgrade ? " (major upgrade allowed)" : "");

    // Collect current packages
    PackageMap *current_packages = collect_current_packages(elm_json);
    if (!current_packages) {
        return SOLVER_INVALID_PACKAGE;
    }

    // Check if package needs to be downloaded from registry
    if (!state->online && !cache_registry_exists(state->cache)) {
        log_error("Offline mode but no cached registry");
        package_map_free(current_packages);
        return SOLVER_NO_OFFLINE_SOLUTION;
    }

    // Ensure registry is available
    if (!cache_registry_exists(state->cache)) {
        log_debug("Downloading registry");
        if (!cache_download_registry(state->cache)) {
            package_map_free(current_packages);
            return SOLVER_NETWORK_ERROR;
        }
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

SolverResult solver_remove_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    InstallPlan **out_plan
) {
    if (!state || !elm_json || !author || !name || !out_plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    *out_plan = NULL;

    log_debug("Removing package: %s/%s", author, name);

    /* Find the package in elm.json */
    Package *target_pkg = NULL;
    bool is_direct = false;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        target_pkg = package_map_find(elm_json->dependencies_direct, author, name);
        if (target_pkg) {
            is_direct = true;
        }
        
        if (!target_pkg) {
            target_pkg = package_map_find(elm_json->dependencies_indirect, author, name);
        }
        
        if (!target_pkg) {
            target_pkg = package_map_find(elm_json->dependencies_test_direct, author, name);
            if (target_pkg) {
                is_direct = true;
            }
        }
        
        if (!target_pkg) {
            target_pkg = package_map_find(elm_json->dependencies_test_indirect, author, name);
        }
    } else {
        target_pkg = package_map_find(elm_json->package_dependencies, author, name);
        if (target_pkg) {
            is_direct = true;
        }
        
        if (!target_pkg) {
            target_pkg = package_map_find(elm_json->package_test_dependencies, author, name);
            if (target_pkg) {
                is_direct = true;
            }
        }
    }

    if (!target_pkg) {
        log_error("Package %s/%s is not in your elm.json", author, name);
        return SOLVER_INVALID_PACKAGE;
    }

    /* Create the install plan */
    InstallPlan *plan = install_plan_create();
    if (!plan) {
        return SOLVER_INVALID_PACKAGE;
    }

    /* Always add the target package to the removal plan */
    install_plan_add_change(plan, author, name, target_pkg->version, NULL);

    /* If removing an indirect dependency, just remove that one package.
     * If removing a direct dependency from an application, we need to find orphaned indirect deps.
     */
    if (is_direct && elm_json->type == ELM_PROJECT_APPLICATION) {
        /* For applications: create a temporary elm.json with the target removed,
         * then re-solve to find what the new indirect dependencies should be.
         * Any current indirect dependency not in the new solution is orphaned.
         */
        
        /* Collect current indirect dependencies */
        PackageMap *current_indirect = package_map_create();
        if (!current_indirect) {
            install_plan_free(plan);
            return SOLVER_INVALID_PACKAGE;
        }

        if (elm_json->dependencies_indirect) {
            for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_indirect->packages[i];
                package_map_add(current_indirect, pkg->author, pkg->name, pkg->version);
            }
        }
        if (elm_json->dependencies_test_indirect) {
            for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
                package_map_add(current_indirect, pkg->author, pkg->name, pkg->version);
            }
        }

        /* For now, we can't reliably compute which indirect dependencies become orphaned
         * without re-running the full solver (which requires downloading packages).
         * So we'll just remove the direct dependency and leave indirect deps alone.
         * The user can run 'wrap install' afterward to clean up if needed.
         */
        package_map_free(current_indirect);
    }

    *out_plan = plan;
    return SOLVER_OK;
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

    // Collect current packages
    PackageMap *current_packages = collect_current_packages(elm_json);
    if (!current_packages) {
        return SOLVER_INVALID_PACKAGE;
    }

    // Check if package needs to be downloaded from registry
    if (!state->online && !cache_registry_exists(state->cache)) {
        log_error("Offline mode but no cached registry");
        package_map_free(current_packages);
        return SOLVER_NO_OFFLINE_SOLUTION;
    }

    // Ensure registry is available
    if (!cache_registry_exists(state->cache)) {
        log_debug("Downloading registry");
        if (!cache_download_registry(state->cache)) {
            package_map_free(current_packages);
            return SOLVER_NETWORK_ERROR;
        }
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
            /* Don't add any root constraints - let solver pick latest versions */
            /* TODO: This is too aggressive. We should still respect elm-version constraints */
            root_ok = true;
        } else {
            /* For minor upgrades, use upgradable within major strategy */
            log_debug("Using upgradable within major version strategy");
            root_ok = build_roots_strategy_upgradable_app(pg_ctx, elm_json);
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
            char version_str[32];
            snprintf(version_str, sizeof(version_str), "%d.%d.%d",
                     selected_ver.major, selected_ver.minor, selected_ver.patch);

            const char *pkg_author = pg_ctx->authors[i];
            const char *pkg_name = pg_ctx->names[i];

            // Find if it was already in current packages
            Package *existing = package_map_find(current_packages, pkg_author, pkg_name);
            const char *old_ver = existing ? existing->version : NULL;

            log_debug("Package[%d] %s/%s: old=%s new=%s", i, pkg_author, pkg_name,
                      old_ver ? old_ver : "NULL", version_str);

            // Only add to plan if it changed
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

