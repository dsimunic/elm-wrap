#include "solver_common.h"
#include "../solver.h"
#include "../install_env.h"
#include "../alloc.h"
#include "../log.h"
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
PackageMap* collect_current_packages(const ElmJson *elm_json) {
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

            /* Must have same major and minor, but patch can be higher */
            return (major == req_major &&
                    minor == req_minor &&
                    patch >= req_patch);
        }

        case CONSTRAINT_UNTIL_NEXT_MAJOR: {
            int major, minor, patch;
            int req_major, req_minor, req_patch;

            parse_version(version, &major, &minor, &patch);
            parse_version(constraint->exact_version, &req_major, &req_minor, &req_patch);

            /* Must have same major, but minor and patch can be higher */
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

/* Package removal */
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
