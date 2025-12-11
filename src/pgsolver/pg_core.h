#ifndef PG_CORE_H
#define PG_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include "../commands/package/package_common.h"

/* Basic version representation */
typedef struct {
    int major;
    int minor;
    int patch;
} PgVersion;

/* Bound on a version range (may be unbounded) */
typedef struct {
    PgVersion v;
    bool inclusive;
    bool unbounded;
} PgBound;

/* Version range (possibly empty, possibly unbounded) */
typedef struct {
    PgBound lower;
    PgBound upper;
    bool is_empty;
} PgVersionRange;

/* Internal package id used by the solver */
typedef int PgPackageId;

/* Dependency provider interface (Elm-specific wiring supplies this) */
typedef struct PgDependencyProvider {
    /*
     * Enumerate all known versions for a package, newest first.
     * Returns the number of versions written to out_versions (<= out_capacity),
     * or -1 on error.
     */
    int (*get_versions)(
        void *ctx,
        PgPackageId pkg,
        PgVersion *out_versions,
        size_t out_capacity
    );

    /*
     * Load dependencies for a specific (pkg, version).
     * Fills out_pkgs[i], out_ranges[i] for i in [0, return_value).
     * Returns number of dependencies (<= out_capacity), or -1 on error.
     */
    int (*get_dependencies)(
        void *ctx,
        PgPackageId pkg,
        PgVersion version,
        PgPackageId *out_pkgs,
        PgVersionRange *out_ranges,
        size_t out_capacity
    );
} PgDependencyProvider;

/* Solver status codes */
typedef enum {
    PG_SOLVER_OK = 0,
    PG_SOLVER_NO_SOLUTION,
    PG_SOLVER_INTERNAL_ERROR
} PgSolverStatus;

/* Opaque solver state */
typedef struct PgSolver PgSolver;

/* Version utilities */
bool pg_version_parse(const char *s, PgVersion *out);
int pg_version_compare(PgVersion a, PgVersion b);

PgVersionRange pg_range_any(void);
PgVersionRange pg_range_exact(PgVersion v);
PgVersionRange pg_range_until_next_minor(PgVersion v);
PgVersionRange pg_range_until_next_major(PgVersion v);
PgVersionRange pg_range_intersect(PgVersionRange a, PgVersionRange b);
bool pg_range_contains(PgVersionRange range, PgVersion v);

/* Solver lifecycle */
PgSolver *pg_solver_new(
    PgDependencyProvider provider,
    void *provider_ctx,
    PgPackageId root_pkg,
    PgVersion root_version
);

void pg_solver_free(PgSolver *solver);

/* Add a root-level dependency constraint before solving. */
bool pg_solver_add_root_dependency(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange range
);

/* Main solving entry point (PubGrub core loop) */
PgSolverStatus pg_solver_solve(PgSolver *solver);

/*
 * Query the selected version for a package after pg_solver_solve.
 * Returns true and writes to out_version if a version was selected.
 */
bool pg_solver_get_selected_version(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersion *out_version
);

/* Statistics */

typedef struct {
    int cache_hits;
    int cache_misses;
    int decisions;
    int propagations;
    int conflicts;
} PgSolverStats;

/*
 * Get solver statistics after pg_solver_solve.
 */
void pg_solver_get_stats(PgSolver *solver, PgSolverStats *out_stats);

/* Error reporting */

/* Callback for resolving package IDs to human-readable names */
typedef const char *(*PgPackageNameResolver)(void *ctx, PgPackageId pkg);

/*
 * Generate a human-readable error message explaining why solving failed.
 * Should only be called after pg_solver_solve returns PG_SOLVER_NO_SOLUTION.
 *
 * solver: The solver that failed to find a solution
 * name_resolver: Callback to convert package IDs to names
 * name_ctx: Context passed to name_resolver
 * out_buffer: Buffer to write the error message
 * buffer_size: Size of out_buffer
 *
 * Returns true on success, false on error (e.g., buffer too small)
 */
bool pg_solver_explain_failure(
    PgSolver *solver,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    char *out_buffer,
    size_t buffer_size
);

/* ========================================================================
 * Conversion functions between PgVersion and Version (from package_common.h)
 * ======================================================================== */

static inline PgVersion pg_version_from_version(Version v) {
    PgVersion pv;
    pv.major = (int)v.major;
    pv.minor = (int)v.minor;
    pv.patch = (int)v.patch;
    return pv;
}

static inline Version pg_version_to_version(PgVersion pv) {
    Version v;
    v.major = (uint16_t)(pv.major >= 0 ? pv.major : 0);
    v.minor = (uint16_t)(pv.minor >= 0 ? pv.minor : 0);
    v.patch = (uint16_t)(pv.patch >= 0 ? pv.patch : 0);
    return v;
}

static inline PgVersionRange pg_range_from_version_range(VersionRange r) {
    PgVersionRange pr;
    pr.lower.v = pg_version_from_version(r.lower.v);
    pr.lower.inclusive = r.lower.inclusive;
    pr.lower.unbounded = r.lower.unbounded;
    pr.upper.v = pg_version_from_version(r.upper.v);
    pr.upper.inclusive = r.upper.inclusive;
    pr.upper.unbounded = r.upper.unbounded;
    pr.is_empty = r.is_empty;
    return pr;
}

static inline VersionRange pg_range_to_version_range(PgVersionRange pr) {
    VersionRange r;
    r.lower.v = pg_version_to_version(pr.lower.v);
    r.lower.inclusive = pr.lower.inclusive;
    r.lower.unbounded = pr.lower.unbounded;
    r.upper.v = pg_version_to_version(pr.upper.v);
    r.upper.inclusive = pr.upper.inclusive;
    r.upper.unbounded = pr.upper.unbounded;
    r.is_empty = pr.is_empty;
    return r;
}

#endif /* PG_CORE_H */
