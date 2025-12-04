#ifndef SOLVER_COMMON_H
#define SOLVER_COMMON_H

#include "../solver.h"
#include "../elm_json.h"
#include <stdbool.h>

/* Solver strategies for package installations */
typedef enum {
    STRATEGY_EXACT_ALL,                        /* Pin all existing dependencies to exact versions */
    STRATEGY_EXACT_DIRECT_UPGRADABLE_INDIRECT, /* Pin direct deps, allow indirect to upgrade */
    STRATEGY_UPGRADABLE_WITHIN_MAJOR,          /* Allow upgrades within major version */
    STRATEGY_CROSS_MAJOR_FOR_TARGET            /* Allow cross-major upgrade for target package */
} SolverStrategy;

/* InstallPlan operations */
InstallPlan* install_plan_create(void);
void install_plan_free(InstallPlan *plan);
bool install_plan_add_change(InstallPlan *plan, const char *author, const char *name,
                             const char *old_version, const char *new_version);

/* Helper to collect all current packages */
PackageMap* collect_current_packages(const ElmJson *elm_json);

/* Solver state operations */
SolverState* solver_init(struct InstallEnv *install_env, bool online);
void solver_free(SolverState *state);

/* Constraint operations */
Constraint* constraint_create_exact(const char *version);
Constraint* constraint_create_until_next_minor(const char *version);
Constraint* constraint_create_until_next_major(const char *version);
Constraint* constraint_create_any(void);
void constraint_free(Constraint *constraint);

/* Version comparison */
int version_compare(const char *v1, const char *v2);
bool version_satisfies(const char *version, Constraint *constraint);

/* Package removal */
SolverResult solver_remove_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    InstallPlan **out_plan
);

#endif /* SOLVER_COMMON_H */
