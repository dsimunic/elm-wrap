#ifndef SOLVER_H
#define SOLVER_H

#include <stdbool.h>
#include "elm_json.h"
#include "cache.h"

struct InstallEnv;

/* Version constraint types */
typedef enum {
    CONSTRAINT_EXACT,
    CONSTRAINT_UNTIL_NEXT_MINOR,
    CONSTRAINT_UNTIL_NEXT_MAJOR,
    CONSTRAINT_ANY
} ConstraintType;

/* Version constraint */
typedef struct {
    ConstraintType type;
    char *exact_version;  // For CONSTRAINT_EXACT
} Constraint;

/* Solver result */
typedef enum {
    SOLVER_OK,
    SOLVER_NO_SOLUTION,
    SOLVER_NO_OFFLINE_SOLUTION,
    SOLVER_NETWORK_ERROR,
    SOLVER_INVALID_PACKAGE
} SolverResult;

/* Package change */
typedef struct {
    char *author;
    char *name;
    char *old_version;  // NULL for adds
    char *new_version;
} PackageChange;

/* Install plan */
typedef struct {
    PackageChange *changes;
    int count;
    int capacity;
} InstallPlan;

/* Solver state */
typedef struct {
    CacheConfig *cache;
    struct InstallEnv *install_env;
    bool online;
} SolverState;

/* Solver operations */
SolverState* solver_init(struct InstallEnv *install_env, bool online);
void solver_free(SolverState *state);

/* Install plan operations */
InstallPlan* install_plan_create(void);
void install_plan_free(InstallPlan *plan);
bool install_plan_add_change(InstallPlan *plan, const char *author, const char *name, const char *old_version, const char *new_version);

/* Constraint operations */
Constraint* constraint_create_exact(const char *version);
Constraint* constraint_create_until_next_minor(const char *version);
Constraint* constraint_create_until_next_major(const char *version);
Constraint* constraint_create_any(void);
void constraint_free(Constraint *constraint);

/* Version operations */
bool version_satisfies(const char *version, Constraint *constraint);
int version_compare(const char *v1, const char *v2);

/* Solver functions */
SolverResult solver_add_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    bool is_test_dependency,
    bool major_upgrade,
    InstallPlan **out_plan
);

SolverResult solver_remove_package(
    SolverState *state,
    const ElmJson *elm_json,
    const char *author,
    const char *name,
    InstallPlan **out_plan
);

SolverResult solver_upgrade_all(
    SolverState *state,
    const ElmJson *elm_json,
    bool major_upgrade,
    InstallPlan **out_plan
);

/* Registry operations (stubbed) */
char** solver_get_available_versions(SolverState *state, const char *author, const char *name, int *count);
void solver_free_versions(char **versions, int count);

#endif /* SOLVER_H */
