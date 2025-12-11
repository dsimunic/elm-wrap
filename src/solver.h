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
    bool upgrade_all,
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

/*
 * Multi-package validation structures
 */

/* Result of validating a single package */
typedef struct {
    const char *author;
    const char *name;
    bool exists;           /* Package found in registry */
    bool valid_name;       /* Name format is valid (author/name) */
    const char *error_msg; /* Human-readable error if failed */
} PackageValidationResult;

/* Collection of validation results for multiple packages */
typedef struct {
    PackageValidationResult *results;
    int count;
    int valid_count;       /* Number of packages that passed validation */
    int invalid_count;     /* Number of packages that failed */
} MultiPackageValidation;

/* Free multi-package validation results */
void multi_package_validation_free(MultiPackageValidation *validation);

/**
 * Add multiple packages to the project.
 *
 * This function:
 * 1. Validates ALL package names upfront (format check)
 * 2. Checks ALL packages exist in registry before solving
 * 3. Reports all errors at once (not fail-fast)
 * 4. If all valid, calls solver_add_package() for each
 * 5. Combines results into single InstallPlan
 *
 * @param state        Initialized solver state
 * @param elm_json     Current project elm.json
 * @param packages     Array of "author/name" strings
 * @param count        Number of packages
 * @param is_test      Install as test dependencies
 * @param upgrade_all  Allow upgrading existing deps
 * @param out_plan     Combined install plan (caller must free)
 * @param out_validation Validation results (caller must free, may be NULL)
 * @return SOLVER_OK if all packages resolved, error code otherwise
 */
SolverResult solver_add_packages(
    SolverState *state,
    const ElmJson *elm_json,
    const char **packages,
    int count,
    bool is_test,
    bool upgrade_all,
    InstallPlan **out_plan,
    MultiPackageValidation **out_validation
);

/**
 * Merge source install plan into destination, deduplicating changes.
 * If the same package appears in both plans, keeps the one from dest.
 *
 * @param dest   Destination plan to merge into
 * @param source Source plan to merge from
 */
void install_plan_merge(InstallPlan *dest, const InstallPlan *source);

#endif /* SOLVER_H */
