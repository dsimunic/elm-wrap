#include "pg_core.h"
#include "solver_common.h"
#include "../alloc.h"
#include "../constants.h"
#include "../log.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

/* Internal core types (kept private to this file) */

typedef struct {
    PgPackageId pkg;
    PgVersionRange range;
    bool positive;
} PgTerm;

typedef enum {
    PG_REASON_DEPENDENCY,
    PG_REASON_NO_VERSIONS,
    PG_REASON_ROOT,
    PG_REASON_INTERNAL
} PgIncompatibilityReason;

typedef struct PgIncompatibility {
    PgTerm *terms;
    size_t term_count;
    PgIncompatibilityReason reason;

    struct PgIncompatibility **causes;
    size_t cause_count;
    bool attached;
} PgIncompatibility;

typedef struct {
    PgPackageId pkg;
    PgVersionRange range;
    bool positive;
    bool decided;
    int decision_level;
    PgIncompatibility *cause;
} PgAssignment;

typedef struct {
    PgAssignment *items;
    size_t count;
    size_t capacity;
} PgAssignmentTrail;

typedef struct {
    PgIncompatibility **items;
    size_t count;
    size_t capacity;
} PgPkgIncompatList;

typedef struct {
    bool used;
    bool has_decision;
    PgVersion decision_version;
} PgPkgState;

typedef struct {
    bool found;
    int available_count;
    PgVersion newest;
} PgDecisionEval;

typedef enum {
    PG_TERM_STATE_SATISFIED,
    PG_TERM_STATE_CONTRADICTED,
    PG_TERM_STATE_INCONCLUSIVE
} PgTermState;

struct PgSolver {
    PgDependencyProvider provider;
    void *provider_ctx;
    PgPackageId root_pkg;
    PgVersion root_version;

    PgAssignmentTrail trail;

    PgPkgState *pkg_states;
    int pkg_state_capacity;

    PgIncompatibility **incompatibilities;
    size_t incompatibility_count;
    size_t incompatibility_capacity;
    PgPkgIncompatList *pkg_incompat_lists;
    int pkg_incompat_capacity;

    int current_decision_level;

    PgPackageId *changed_pkgs;
    size_t changed_count;
    size_t changed_capacity;

    /* Whether pg_solver_solve has already been run. */
    bool solved;

    /* Root incompatibility when solving fails (for error reporting) */
    PgIncompatibility *root_incompatibility;

    /* Version cache to avoid repeated get_versions() calls */
    PgVersion **cached_versions;  /* Array of version arrays, indexed by package ID */
    int *cached_counts;           /* Count of versions for each package */
    int cache_capacity;           /* Capacity of cache arrays */

    /* Statistics for performance analysis */
    int stats_cache_hits;
    int stats_cache_misses;
    int stats_decisions;
    int stats_propagations;
    int stats_conflicts;
};

#define PG_DECISION_VERSION_BUFFER 128
#define PG_DEPENDENCY_BUFFER 128

static bool pg_solver_ensure_pkg_state(PgSolver *solver, PgPackageId pkg);
static bool pg_solver_enqueue_changed(PgSolver *solver, PgPackageId pkg);
static bool pg_solver_pop_changed(PgSolver *solver, PgPackageId *out_pkg);
static bool pg_solver_ensure_pkg_incompat_capacity(PgSolver *solver, PgPackageId pkg);
static bool pg_solver_backtrack_to_level(PgSolver *solver, int level);
static bool pg_solver_track_incompatibility(PgSolver *solver, PgIncompatibility *inc);
static PgIncompatibility *pg_solver_add_incompatibility(
    PgSolver *solver,
    const PgTerm *terms,
    size_t term_count,
    PgIncompatibilityReason reason,
    PgIncompatibility **causes,
    size_t cause_count
);
static bool pg_solver_attach_incompatibility(PgSolver *solver, PgIncompatibility *inc);
static PgSolverStatus pg_unit_propagate(PgSolver *solver, PgIncompatibility **out_conflict);
static bool pg_solver_version_is_forbidden(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersion version
);
static PgSolverStatus pg_solver_evaluate_candidate(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange required,
    PgDecisionEval *out_eval,
    PgIncompatibility **out_conflict
);
static PgSolverStatus pg_solver_register_dependencies(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange decision_range,
    PgVersion version
);
static PgSolverStatus pg_make_decision(
    PgSolver *solver,
    bool *out_made_decision,
    PgIncompatibility **out_conflict
);
static PgIncompatibility *pg_resolve_conflict(
    PgSolver *solver,
    PgIncompatibility *conflict,
    int *out_backjump_level
);

/* Internal helpers */
static int pg_version_cmp_internal(const PgVersion *a, const PgVersion *b) {
    if (a->major != b->major) {
        return (a->major < b->major) ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return (a->minor < b->minor) ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return (a->patch < b->patch) ? -1 : 1;
    }
    return 0;
}

static void pg_trail_init(PgAssignmentTrail *trail) {
    trail->items = NULL;
    trail->count = 0;
    trail->capacity = 0;
}

static void pg_trail_free(PgAssignmentTrail *trail) {
    arena_free(trail->items);
    trail->items = NULL;
    trail->count = 0;
    trail->capacity = 0;
}

static bool pg_trail_push(PgAssignmentTrail *trail, PgAssignment assignment) {
    if (trail->count >= trail->capacity) {
        size_t new_capacity = trail->capacity == 0 ? 8 : trail->capacity * 2;
        PgAssignment *new_items = (PgAssignment *)arena_realloc(
            trail->items, new_capacity * sizeof(PgAssignment));
        if (!new_items) {
            return false;
        }
        trail->items = new_items;
        trail->capacity = new_capacity;
    }
    trail->items[trail->count] = assignment;
    trail->count++;
    return true;
}

static bool pg_range_is_exact(PgVersionRange range, PgVersion *out_version) {
    if (range.lower.unbounded || range.upper.unbounded) {
        return false;
    }
    if (!range.lower.inclusive || !range.upper.inclusive) {
        return false;
    }
    if (pg_version_cmp_internal(&range.lower.v, &range.upper.v) != 0) {
        return false;
    }
    if (out_version) {
        *out_version = range.lower.v;
    }
    return true;
}

static bool pg_solver_compute_positive_range(
    PgSolver *solver,
    PgPackageId pkg,
    bool *out_has_range,
    PgVersionRange *out_range
) {
    if (!solver || pkg < 0) {
        return false;
    }

    bool has_range = false;
    PgVersionRange combined = pg_range_any();
    combined.is_empty = false;

    for (size_t i = 0; i < solver->trail.count; i++) {
        PgAssignment *assignment = &solver->trail.items[i];
        if (assignment->pkg != pkg || !assignment->positive) {
            continue;
        }

        if (!has_range) {
            combined = assignment->range;
            has_range = true;
        } else {
            combined = pg_range_intersect(combined, assignment->range);
            if (combined.is_empty) {
                return false;
            }
        }
    }

    if (out_has_range) {
        *out_has_range = has_range;
    }
    if (has_range && out_range) {
        *out_range = combined;
    }

    return true;
}

static bool pg_solver_enqueue_changed(PgSolver *solver, PgPackageId pkg) {
    if (!solver || pkg < 0) {
        return false;
    }

    if (solver->changed_count >= solver->changed_capacity) {
        size_t new_capacity = solver->changed_capacity == 0
            ? 8
            : solver->changed_capacity * 2;
        PgPackageId *new_items = (PgPackageId *)arena_realloc(
            solver->changed_pkgs,
            new_capacity * sizeof(PgPackageId)
        );
        if (!new_items) {
            return false;
        }
        solver->changed_pkgs = new_items;
        solver->changed_capacity = new_capacity;
    }

    solver->changed_pkgs[solver->changed_count] = pkg;
    solver->changed_count++;
    return true;
}

static bool pg_solver_pop_changed(PgSolver *solver, PgPackageId *out_pkg) {
    if (!solver || !out_pkg) {
        return false;
    }
    if (solver->changed_count == 0) {
        return false;
    }

    solver->changed_count--;
    *out_pkg = solver->changed_pkgs[solver->changed_count];
    return true;
}

static bool pg_solver_ensure_pkg_incompat_capacity(PgSolver *solver, PgPackageId pkg) {
    if (!solver || pkg < 0) {
        return false;
    }

    if (pkg < solver->pkg_incompat_capacity) {
        return true;
    }

    int new_capacity = solver->pkg_incompat_capacity == 0
        ? (pkg + 1)
        : solver->pkg_incompat_capacity;
    while (new_capacity <= pkg) {
        new_capacity *= 2;
    }

    PgPkgIncompatList *new_lists = (PgPkgIncompatList *)arena_realloc(
        solver->pkg_incompat_lists,
        (size_t)new_capacity * sizeof(PgPkgIncompatList)
    );
    if (!new_lists) {
        return false;
    }

    for (int i = solver->pkg_incompat_capacity; i < new_capacity; i++) {
        new_lists[i].items = NULL;
        new_lists[i].count = 0;
        new_lists[i].capacity = 0;
    }

    solver->pkg_incompat_lists = new_lists;
    solver->pkg_incompat_capacity = new_capacity;
    return true;
}

static bool pg_pkg_incompat_list_append(
    PgPkgIncompatList *list,
    PgIncompatibility *inc
) {
    if (!list) {
        return false;
    }

    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        PgIncompatibility **new_items = (PgIncompatibility **)arena_realloc(
            list->items,
            new_capacity * sizeof(PgIncompatibility *)
        );
        if (!new_items) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count] = inc;
    list->count++;
    return true;
}

static void pg_pkg_incompat_list_remove(
    PgPkgIncompatList *list,
    PgIncompatibility *inc
) {
    if (!list || list->count == 0 || !inc) {
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i] == inc) {
            list->items[i] = list->items[list->count - 1];
            list->count--;
            return;
        }
    }
}

static void pg_incompatibility_free(PgIncompatibility *inc) {
    if (!inc) {
        return;
    }
    arena_free(inc->terms);
    arena_free(inc->causes);
    arena_free(inc);
}

static PgIncompatibility *pg_incompatibility_new(
    const PgTerm *terms,
    size_t term_count,
    PgIncompatibilityReason reason,
    PgIncompatibility **causes,
    size_t cause_count
) {
    PgIncompatibility *inc = (PgIncompatibility *)arena_malloc(sizeof(PgIncompatibility));
    if (!inc) {
        return NULL;
    }

    inc->terms = NULL;
    inc->causes = NULL;
    inc->term_count = term_count;
    inc->reason = reason;
    inc->cause_count = cause_count;
    inc->attached = false;

    if (term_count > 0) {
        inc->terms = (PgTerm *)arena_malloc(term_count * sizeof(PgTerm));
        if (!inc->terms) {
            pg_incompatibility_free(inc);
            return NULL;
        }
        memcpy(inc->terms, terms, term_count * sizeof(PgTerm));
    }

    if (cause_count > 0) {
        inc->causes = (PgIncompatibility **)arena_malloc(
            cause_count * sizeof(PgIncompatibility *)
        );
        if (!inc->causes) {
            pg_incompatibility_free(inc);
            return NULL;
        }
        memcpy(inc->causes, causes, cause_count * sizeof(PgIncompatibility *));
    }

    return inc;
}

static bool pg_solver_track_incompatibility(PgSolver *solver, PgIncompatibility *inc) {
    if (!solver || !inc) {
        return false;
    }

    if (solver->incompatibility_count >= solver->incompatibility_capacity) {
        size_t new_capacity = solver->incompatibility_capacity == 0
            ? 8
            : solver->incompatibility_capacity * 2;
        PgIncompatibility **new_list = (PgIncompatibility **)arena_realloc(
            solver->incompatibilities,
            new_capacity * sizeof(PgIncompatibility *)
        );
        if (!new_list) {
            return false;
        }
        solver->incompatibilities = new_list;
        solver->incompatibility_capacity = new_capacity;
    }

    solver->incompatibilities[solver->incompatibility_count] = inc;
    solver->incompatibility_count++;
    return true;
}

static bool pg_solver_attach_incompatibility(PgSolver *solver, PgIncompatibility *inc) {
    if (!solver || !inc) {
        return false;
    }
    if (inc->attached) {
        return true;
    }

    size_t attached = 0;
    for (size_t i = 0; i < inc->term_count; i++) {
        PgPackageId pkg = inc->terms[i].pkg;
        if (!pg_solver_ensure_pkg_state(solver, pkg)) {
            goto fail;
        }
        if (!pg_solver_ensure_pkg_incompat_capacity(solver, pkg)) {
            goto fail;
        }
        if (!pg_pkg_incompat_list_append(
                &solver->pkg_incompat_lists[pkg],
                inc)) {
            goto fail;
        }
        attached++;
        if (!pg_solver_enqueue_changed(solver, pkg)) {
            goto fail;
        }
    }

    inc->attached = true;
    return true;

fail:
    while (attached > 0) {
        attached--;
        PgPackageId pkg = inc->terms[attached].pkg;
        if (pkg < solver->pkg_incompat_capacity) {
            pg_pkg_incompat_list_remove(
                &solver->pkg_incompat_lists[pkg],
                inc
            );
        }
    }
    return false;
}

static bool pg_lower_bound_ge(PgBound a, PgBound b) {
    if (b.unbounded) {
        return true;
    }
    if (a.unbounded) {
        return false;
    }

    int cmp = pg_version_cmp_internal(&a.v, &b.v);
    if (cmp > 0) {
        return true;
    }
    if (cmp < 0) {
        return false;
    }

    if (!b.inclusive && a.inclusive) {
        return false;
    }
    return true;
}

static bool pg_upper_bound_le(PgBound a, PgBound b) {
    if (b.unbounded) {
        return true;
    }
    if (a.unbounded) {
        return false;
    }

    int cmp = pg_version_cmp_internal(&a.v, &b.v);
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }

    if (!b.inclusive && a.inclusive) {
        return false;
    }
    return true;
}

static bool pg_range_subset(PgVersionRange a, PgVersionRange b) {
    if (a.is_empty) {
        return true;
    }
    return pg_lower_bound_ge(a.lower, b.lower) &&
        pg_upper_bound_le(a.upper, b.upper);
}

static PgTermState pg_term_state(PgSolver *solver, PgTerm term) {
    if (!solver) {
        return PG_TERM_STATE_INCONCLUSIVE;
    }

    /* Check all assignments for this package to determine term state */
    bool has_positive = false;
    PgVersionRange positive_range = pg_range_any();
    bool has_negative = false;

    for (size_t i = 0; i < solver->trail.count; i++) {
        PgAssignment *assignment = &solver->trail.items[i];
        if (assignment->pkg != term.pkg) {
            continue;
        }

        if (assignment->positive) {
            if (!has_positive) {
                positive_range = assignment->range;
                has_positive = true;
            } else {
                positive_range = pg_range_intersect(positive_range, assignment->range);
            }
        } else {
            /* Negative assignment: check if it contradicts the term */
            if (term.positive) {
                /* Positive term vs negative assignment: if they overlap, contradiction */
                PgVersionRange overlap = pg_range_intersect(term.range, assignment->range);
                if (!overlap.is_empty) {
                    /* The negative assignment forbids part of the term range */
                    has_negative = true;
                }
            }
        }
    }

    /* For positive terms */
    if (term.positive) {
        if (has_positive) {
            PgVersionRange intersection = pg_range_intersect(positive_range, term.range);
            if (pg_range_subset(positive_range, term.range)) {
                return PG_TERM_STATE_SATISFIED;
            }
            if (intersection.is_empty || has_negative) {
                return PG_TERM_STATE_CONTRADICTED;
            }
            return PG_TERM_STATE_INCONCLUSIVE;
        }
        if (has_negative) {
            return PG_TERM_STATE_CONTRADICTED;
        }
        return PG_TERM_STATE_INCONCLUSIVE;
    }

    /* For negative terms */
    if (has_positive) {
        PgVersionRange intersection = pg_range_intersect(positive_range, term.range);
        if (intersection.is_empty) {
            return PG_TERM_STATE_SATISFIED;
        }
        if (pg_range_subset(positive_range, term.range)) {
            return PG_TERM_STATE_CONTRADICTED;
        }
        return PG_TERM_STATE_INCONCLUSIVE;
    }

    return PG_TERM_STATE_INCONCLUSIVE;
}

static bool pg_assignment_from_term_negation(
    PgTerm term,
    PgIncompatibility *cause,
    int decision_level,
    PgAssignment *out
) {
    if (!out) {
        return false;
    }

    out->pkg = term.pkg;
    out->range = term.range;
    out->positive = !term.positive;
    out->decided = false;
    out->decision_level = decision_level;
    out->cause = cause;
    return true;
}

static PgIncompatibility *pg_solver_add_incompatibility(
    PgSolver *solver,
    const PgTerm *terms,
    size_t term_count,
    PgIncompatibilityReason reason,
    PgIncompatibility **causes,
    size_t cause_count
) {
    if (!solver || !terms || term_count == 0) {
        return NULL;
    }

    PgIncompatibility *inc = pg_incompatibility_new(
        terms,
        term_count,
        reason,
        causes,
        cause_count
    );
    if (!inc) {
        return NULL;
    }

    if (!pg_solver_track_incompatibility(solver, inc)) {
        pg_incompatibility_free(inc);
        return NULL;
    }

    if (!pg_solver_attach_incompatibility(solver, inc)) {
        solver->incompatibility_count--;
        pg_incompatibility_free(inc);
        return NULL;
    }

    return inc;
}

static bool pg_solver_push_assignment(PgSolver *solver, PgAssignment assignment) {
    if (!solver) {
        return false;
    }

    if (!pg_solver_ensure_pkg_state(solver, assignment.pkg)) {
        return false;
    }

    if (!pg_trail_push(&solver->trail, assignment)) {
        return false;
    }

    PgPkgState *state = &solver->pkg_states[assignment.pkg];
    state->used = true;

    if (assignment.decided && assignment.positive) {
        PgVersion version;
        if (pg_range_is_exact(assignment.range, &version)) {
            state->has_decision = true;
            state->decision_version = version;
        }
    }

    if (!pg_solver_enqueue_changed(solver, assignment.pkg)) {
        return false;
    }

    return true;
}

static bool pg_solver_backtrack_to_level(PgSolver *solver, int level) {
    if (!solver) {
        return false;
    }

    if (level < 0) {
        level = 0;
    }

    while (solver->trail.count > 0) {
        PgAssignment *assignment = &solver->trail.items[solver->trail.count - 1];
        if (assignment->decision_level <= level) {
            break;
        }
        solver->trail.count--;
    }

    for (int i = 0; i < solver->pkg_state_capacity; i++) {
        solver->pkg_states[i].used = false;
        solver->pkg_states[i].has_decision = false;
        solver->pkg_states[i].decision_version.major = 0;
        solver->pkg_states[i].decision_version.minor = 0;
        solver->pkg_states[i].decision_version.patch = 0;
    }

    for (size_t i = 0; i < solver->trail.count; i++) {
        PgAssignment *assignment = &solver->trail.items[i];
        if (assignment->pkg >= solver->pkg_state_capacity) {
            continue;
        }
        PgPkgState *state = &solver->pkg_states[assignment->pkg];
        state->used = true;
        if (assignment->decided && assignment->positive) {
            PgVersion version;
            if (pg_range_is_exact(assignment->range, &version)) {
                state->has_decision = true;
                state->decision_version = version;
            }
        }
    }

    solver->current_decision_level = level;
    solver->changed_count = 0;
    for (size_t i = 0; i < solver->trail.count; i++) {
        if (!pg_solver_enqueue_changed(solver, solver->trail.items[i].pkg)) {
            return false;
        }
    }
    return true;
}

static bool pg_solver_ensure_pkg_state(PgSolver *solver, PgPackageId pkg) {
    if (!solver || pkg < 0) {
        return false;
    }

    if (pkg >= solver->pkg_state_capacity) {
        int new_capacity = solver->pkg_state_capacity == 0
            ? (pkg + 1)
            : solver->pkg_state_capacity;
        while (new_capacity <= pkg) {
            new_capacity *= 2;
        }

        PgPkgState *new_states = (PgPkgState *)arena_realloc(
            solver->pkg_states,
            (size_t)new_capacity * sizeof(PgPkgState)
        );
        if (!new_states) {
            return false;
        }

        /* Initialize new entries */
        for (int i = solver->pkg_state_capacity; i < new_capacity; i++) {
            new_states[i].used = false;
            new_states[i].has_decision = false;
            new_states[i].decision_version.major = 0;
            new_states[i].decision_version.minor = 0;
            new_states[i].decision_version.patch = 0;
        }

        solver->pkg_states = new_states;
        solver->pkg_state_capacity = new_capacity;
    }

    return true;
}

/* Add a positive version-range requirement for a package.
 *
 * This:
 *   - Ensures per-package state exists.
 *   - Intersects the new range with any existing required range.
 *   - Records an assignment on the trail (for future PubGrub reasoning).
 *
 * Returns false if the combined required range becomes empty or on
 * allocation failure.
 */
static bool pg_solver_add_positive_requirement(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange range,
    bool decided,
    int decision_level,
    PgIncompatibility *cause
) {
    if (!solver) {
        return false;
    }

    bool has_existing = false;
    PgVersionRange existing;
    if (!pg_solver_compute_positive_range(solver, pkg, &has_existing, &existing)) {
        return false;
    }

    if (has_existing) {
        PgVersionRange combined = pg_range_intersect(existing, range);
        if (combined.is_empty) {
            return false;
        }
    }

    /* Record assignment for this specific range. */
    PgAssignment assignment;
    assignment.pkg = pkg;
    assignment.range = range;
    assignment.positive = true;
    assignment.decided = decided;
    assignment.decision_level = decision_level;
    assignment.cause = cause;

    return pg_solver_push_assignment(solver, assignment);
}

static bool pg_solver_version_is_forbidden(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersion version
) {
    if (!solver) {
        return false;
    }

    for (size_t i = 0; i < solver->trail.count; i++) {
        PgAssignment *assignment = &solver->trail.items[i];
        if (assignment->pkg != pkg || assignment->positive) {
            continue;
        }
        if (pg_range_contains(assignment->range, version)) {
            log_trace("pkg=%d forbidden by range %d.%d.%d",
                pkg,
                assignment->range.lower.v.major,
                assignment->range.lower.v.minor,
                assignment->range.lower.v.patch
            );
            return true;
        }
    }

    return false;
}

/* Check if choosing a specific version would immediately satisfy an incompatibility.
 * This prevents the solver from making decisions that will immediately conflict.
 */
static bool pg_solver_version_would_conflict(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersion version
) {
    if (!solver || pkg < 0) {
        return false;
    }

    if (pkg >= solver->pkg_incompat_capacity) {
        return false;
    }

    PgPkgIncompatList *list = &solver->pkg_incompat_lists[pkg];
    for (size_t idx = 0; idx < list->count; idx++) {
        PgIncompatibility *inc = list->items[idx];
        if (!inc || inc->term_count == 0) {
            continue;
        }

        /* Check if this incompatibility would be fully satisfied if we choose this version */
        bool would_satisfy = true;
        bool found_pkg_term = false;

        for (size_t t = 0; t < inc->term_count; t++) {
            PgTerm term = inc->terms[t];

            if (term.pkg == pkg) {
                /* Check if choosing this version would satisfy this term */
                if (term.positive) {
                    /* Positive term: version must be in range */
                    if (!pg_range_contains(term.range, version)) {
                        would_satisfy = false;
                        break;
                    }
                } else {
                    /* Negative term: version must NOT be in range */
                    if (pg_range_contains(term.range, version)) {
                        would_satisfy = false;
                        break;
                    }
                }
                found_pkg_term = true;
            } else {
                /* Check if this term is already satisfied by the current trail */
                PgTermState state = pg_term_state(solver, term);
                if (state != PG_TERM_STATE_SATISFIED) {
                    would_satisfy = false;
                    break;
                }
            }
        }

        if (would_satisfy && found_pkg_term) {
            fprintf(
                stderr,
                "[pg_debug] pkg=%d version %d.%d.%d would satisfy incompatibility\n",
                pkg,
                version.major,
                version.minor,
                version.patch
            );
            return true;
        }
    }

    return false;
}

static PgSolverStatus pg_solver_evaluate_candidate(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange required,
    PgDecisionEval *out_eval,
    PgIncompatibility **out_conflict
) {
    if (!solver || !out_eval) {
        return PG_SOLVER_INTERNAL_ERROR;
    }

    PgIncompatibility **conflict_ptr = out_conflict;
    PgIncompatibility *temp_conflict = NULL;
    if (!conflict_ptr) {
        conflict_ptr = &temp_conflict;
    }

    /* Check cache first */
    PgVersion *versions = NULL;
    int version_count = 0;

    /* Cache hit if pkg is in range AND we've fetched it before (count != -1 means cached) */
    if (pkg >= 0 && pkg < solver->cache_capacity && solver->cached_counts[pkg] != -1) {
        /* Cache hit - use cached versions */
        solver->stats_cache_hits++;
        versions = solver->cached_versions[pkg];
        version_count = solver->cached_counts[pkg];
    } else {
        /* Cache miss - fetch versions and cache them */
        solver->stats_cache_misses++;
        PgVersion temp_versions[PG_DECISION_VERSION_BUFFER];
        version_count = solver->provider.get_versions(
            solver->provider_ctx,
            pkg,
            temp_versions,
            sizeof(temp_versions) / sizeof(temp_versions[0])
        );
        if (version_count < 0) {
            return PG_SOLVER_INTERNAL_ERROR;
        }

        /* Grow cache arrays if needed */
        if (pkg >= solver->cache_capacity) {
            int new_capacity = (pkg + 1) * 2;
            PgVersion **new_cached_versions = (PgVersion **)arena_realloc(
                solver->cached_versions, (size_t)new_capacity * sizeof(PgVersion *));
            int *new_cached_counts = (int *)arena_realloc(
                solver->cached_counts, (size_t)new_capacity * sizeof(int));

            if (!new_cached_versions || !new_cached_counts) {
                arena_free(new_cached_versions);
                arena_free(new_cached_counts);
                return PG_SOLVER_INTERNAL_ERROR;
            }

            /* Initialize new entries (-1 means not cached yet) */
            for (int i = solver->cache_capacity; i < new_capacity; i++) {
                new_cached_versions[i] = NULL;
                new_cached_counts[i] = -1;
            }

            solver->cached_versions = new_cached_versions;
            solver->cached_counts = new_cached_counts;
            solver->cache_capacity = new_capacity;
        }

        /* Allocate and cache the versions */
        if (version_count > 0) {
            solver->cached_versions[pkg] = (PgVersion *)arena_malloc(
                (size_t)version_count * sizeof(PgVersion));
            if (!solver->cached_versions[pkg]) {
                return PG_SOLVER_INTERNAL_ERROR;
            }
            memcpy(solver->cached_versions[pkg], temp_versions,
                   (size_t)version_count * sizeof(PgVersion));
            solver->cached_counts[pkg] = version_count;
            versions = solver->cached_versions[pkg];
        } else {
            /* No versions - cache this fact too */
            solver->cached_versions[pkg] = NULL;
            solver->cached_counts[pkg] = 0;
            versions = NULL;
        }
    }

    int available = 0;
    bool found = false;
    PgVersion chosen;
    chosen.major = 0;
    chosen.minor = 0;
    chosen.patch = 0;

    for (int i = 0; i < version_count; i++) {
        PgVersion v = versions[i];
        if (!pg_range_contains(required, v)) {
            continue;
        }
        if (pg_solver_version_is_forbidden(solver, pkg, v)) {
            continue;
        }
        if (pg_solver_version_would_conflict(solver, pkg, v)) {
            continue;
        }

        if (!found) {
            chosen = v;
            found = true;
        }
        available++;
    }

    if (!found) {
        PgTerm term;
        term.pkg = pkg;
        term.range = required;
        term.positive = true;
        PgIncompatibility *inc = pg_solver_add_incompatibility(
            solver,
            &term,
            1,
            PG_REASON_NO_VERSIONS,
            NULL,
            0
        );
        if (!inc) {
            return PG_SOLVER_INTERNAL_ERROR;
        }
        *conflict_ptr = inc;
        out_eval->found = false;
        out_eval->available_count = 0;
        return PG_SOLVER_OK;
    }

    out_eval->found = true;
    out_eval->available_count = available;
    out_eval->newest = chosen;
    return PG_SOLVER_OK;
}

static PgSolverStatus pg_solver_register_dependencies(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange decision_range,
    PgVersion version
) {
    PgPackageId dep_pkgs[PG_DEPENDENCY_BUFFER];
    PgVersionRange dep_ranges[PG_DEPENDENCY_BUFFER];
    int dep_count = solver->provider.get_dependencies(
        solver->provider_ctx,
        pkg,
        version,
        dep_pkgs,
        dep_ranges,
        sizeof(dep_pkgs) / sizeof(dep_pkgs[0])
    );
    if (dep_count < 0) {
        return PG_SOLVER_INTERNAL_ERROR;
    }

    for (int i = 0; i < dep_count; i++) {
        PgTerm terms[2];
        terms[0].pkg = pkg;
        terms[0].range = decision_range;
        terms[0].positive = true;

        terms[1].pkg = dep_pkgs[i];
        terms[1].range = dep_ranges[i];
        terms[1].positive = false;

        PgIncompatibility *dep_inc = pg_solver_add_incompatibility(
            solver,
            terms,
            2,
            PG_REASON_DEPENDENCY,
            NULL,
            0
        );
        if (!dep_inc) {
            return PG_SOLVER_INTERNAL_ERROR;
        }
    }

    return PG_SOLVER_OK;
}

static PgBound pg_unbounded_bound(void) {
    PgBound b;
    b.v.major = 0;
    b.v.minor = 0;
    b.v.patch = 0;
    b.inclusive = false;
    b.unbounded = true;
    return b;
}

/* Version utilities */
bool pg_version_parse(const char *s, PgVersion *out) {
    if (!s || !out) {
        return false;
    }

    int major = 0;
    int minor = 0;
    int patch = 0;

    if (sscanf(s, "%d.%d.%d", &major, &minor, &patch) != 3) {
        return false;
    }

    out->major = major;
    out->minor = minor;
    out->patch = patch;
    return true;
}

static PgSolverStatus pg_unit_propagate(
    PgSolver *solver,
    PgIncompatibility **out_conflict
) {
    if (!solver) {
        return PG_SOLVER_INTERNAL_ERROR;
    }

    if (out_conflict) {
        *out_conflict = NULL;
    }

    PgPackageId pkg;
    while (pg_solver_pop_changed(solver, &pkg)) {
        if (pkg >= 0 && pkg < solver->pkg_incompat_capacity) {
            PgPkgIncompatList *list = &solver->pkg_incompat_lists[pkg];
            for (ssize_t idx = (ssize_t)list->count - 1; idx >= 0; idx--) {
                PgIncompatibility *inc = list->items[idx];

                size_t inconclusive_index = SIZE_MAX;
                bool multiple_inconclusive = false;
                bool has_contradiction = false;
                size_t satisfied = 0;

                for (size_t t = 0; t < inc->term_count; t++) {
                    PgTermState state = pg_term_state(solver, inc->terms[t]);
                    if (state == PG_TERM_STATE_SATISFIED) {
                        satisfied++;
                    } else if (state == PG_TERM_STATE_INCONCLUSIVE) {
                        if (inconclusive_index == SIZE_MAX) {
                            inconclusive_index = t;
                        } else {
                            multiple_inconclusive = true;
                            break;
                        }
                    } else {
                        has_contradiction = true;
                        break;
                    }
                }

                if (has_contradiction) {
                    continue;
                }

                if (!multiple_inconclusive && inconclusive_index == SIZE_MAX) {
                    if (out_conflict) {
                        *out_conflict = inc;
                    }
                    return PG_SOLVER_OK;
                }

                if (!multiple_inconclusive &&
                    inconclusive_index < inc->term_count &&
                    satisfied == inc->term_count - 1) {
                    PgAssignment derived_assignment;
                    if (!pg_assignment_from_term_negation(
                            inc->terms[inconclusive_index],
                            inc,
                            solver->current_decision_level,
                            &derived_assignment)) {
                        return PG_SOLVER_INTERNAL_ERROR;
                    }
                    if (!pg_solver_push_assignment(solver, derived_assignment)) {
                        return PG_SOLVER_INTERNAL_ERROR;
                    }
                    /* Don't break here - continue checking other incompatibilities
                     * for this package before moving to the next changed package */
                }
            }
        }
    }

    return PG_SOLVER_OK;
}

static PgSolverStatus pg_make_decision(
    PgSolver *solver,
    bool *out_made_decision,
    PgIncompatibility **out_conflict
) {
    if (!solver) {
        return PG_SOLVER_INTERNAL_ERROR;
    }

    if (out_made_decision) {
        *out_made_decision = false;
    }
    if (out_conflict) {
        *out_conflict = NULL;
    }

    PgPackageId best_pkg = -1;
    PgDecisionEval best_eval;
    best_eval.found = false;
    best_eval.available_count = INT_MAX;
    best_eval.newest.major = 0;
    best_eval.newest.minor = 0;
    best_eval.newest.patch = 0;

    for (int pkg = 0; pkg < solver->pkg_state_capacity; pkg++) {
        if (pkg == solver->root_pkg) {
            continue;
        }

        PgPkgState *state = &solver->pkg_states[pkg];
        if (!state->used || state->has_decision) {
            continue;
        }

        bool has_range = false;
        PgVersionRange required;
        if (!pg_solver_compute_positive_range(
                solver,
                pkg,
                &has_range,
                &required)) {
            return PG_SOLVER_INTERNAL_ERROR;
        }

        if (!has_range) {
            continue;
        }

        PgDecisionEval eval;
        PgSolverStatus eval_status = pg_solver_evaluate_candidate(
            solver,
            pkg,
            required,
            &eval,
            out_conflict
        );
        if (eval_status != PG_SOLVER_OK) {
            return eval_status;
        }
        if (out_conflict && *out_conflict) {
            log_trace("pkg=%d no versions available", pkg);
            return PG_SOLVER_OK;
        }
        if (!eval.found) {
            continue;
        }

        if (best_pkg == -1 ||
            eval.available_count < best_eval.available_count ||
            (eval.available_count == best_eval.available_count && pkg < best_pkg)) {
            best_pkg = pkg;
            best_eval = eval;
        }
    }

    if (best_pkg == -1) {
        return PG_SOLVER_OK;
    }

    log_trace("decision candidate pkg=%d (choices=%d)", best_pkg, best_eval.available_count);
    log_trace("pkg=%d choose %d.%d.%d", best_pkg, best_eval.newest.major, best_eval.newest.minor, best_eval.newest.patch);

    int prev_level = solver->current_decision_level;
    solver->current_decision_level = prev_level + 1;

    PgAssignment assignment;
    assignment.pkg = best_pkg;
    assignment.range = pg_range_exact(best_eval.newest);
    assignment.positive = true;
    assignment.decided = true;
    assignment.decision_level = solver->current_decision_level;
    assignment.cause = NULL;

    if (!pg_solver_push_assignment(solver, assignment)) {
        solver->current_decision_level = prev_level;
        return PG_SOLVER_INTERNAL_ERROR;
    }

    PgSolverStatus dep_status = pg_solver_register_dependencies(
        solver,
        best_pkg,
        assignment.range,
        best_eval.newest
    );
    if (dep_status != PG_SOLVER_OK) {
        solver->current_decision_level = prev_level;
        return dep_status;
    }

    if (out_made_decision) {
        *out_made_decision = true;
    }
    return PG_SOLVER_OK;
}

static bool pg_assignment_satisfies_term(
    const PgAssignment *assignment,
    PgTerm term
) {
    if (!assignment || assignment->pkg != term.pkg) {
        return false;
    }

    if (term.positive) {
        if (!assignment->positive) {
            return false;
        }
        return pg_range_subset(assignment->range, term.range);
    }

    if (assignment->positive) {
        PgVersionRange intersection = pg_range_intersect(
            assignment->range,
            term.range
        );
        return intersection.is_empty;
    }

    return pg_range_subset(term.range, assignment->range);
}

static PgAssignment *pg_find_assignment_for_term(
    PgSolver *solver,
    PgTerm term,
    size_t *out_index
) {
    if (!solver) {
        return NULL;
    }

    for (ssize_t i = (ssize_t)solver->trail.count - 1; i >= 0; i--) {
        PgAssignment *assignment = &solver->trail.items[i];
        if (pg_assignment_satisfies_term(assignment, term)) {
            if (out_index) {
                *out_index = (size_t)i;
            }
            return assignment;
        }
    }

    return NULL;
}

static bool pg_incompatibility_find_satisfier(
    PgSolver *solver,
    PgIncompatibility *inc,
    size_t *out_assignment_index,
    size_t *out_term_index
) {
    if (!solver || !inc || inc->term_count == 0) {
        return false;
    }

    bool *satisfied = (bool *)arena_calloc(inc->term_count, sizeof(bool));
    size_t *satisfier_index = (size_t *)arena_malloc(
        inc->term_count * sizeof(size_t)
    );
    if (!satisfied || !satisfier_index) {
        arena_free(satisfied);
        arena_free(satisfier_index);
        return false;
    }

    for (size_t i = 0; i < inc->term_count; i++) {
        satisfier_index[i] = SIZE_MAX;
    }

    bool conflict_satisfied = false;
    size_t assignment_index = SIZE_MAX;
    size_t term_index = SIZE_MAX;

    for (size_t i = 0; i < solver->trail.count; i++) {
        PgAssignment *assignment = &solver->trail.items[i];
        for (size_t t = 0; t < inc->term_count; t++) {
            if (satisfied[t]) {
                continue;
            }
            if (pg_assignment_satisfies_term(assignment, inc->terms[t])) {
                satisfied[t] = true;
                satisfier_index[t] = i;
            }
        }

        bool all = true;
        for (size_t t = 0; t < inc->term_count; t++) {
            if (!satisfied[t]) {
                all = false;
                break;
            }
        }

        if (all) {
            conflict_satisfied = true;
            assignment_index = i;
            for (size_t t = 0; t < inc->term_count; t++) {
                if (satisfier_index[t] == i) {
                    term_index = t;
                    break;
                }
            }
            if (term_index == SIZE_MAX) {
                term_index = 0;
            }
            break;
        }
    }

    arena_free(satisfied);
    arena_free(satisfier_index);

    if (!conflict_satisfied) {
        return false;
    }

    if (out_assignment_index) {
        *out_assignment_index = assignment_index;
    }
    if (out_term_index) {
        *out_term_index = term_index;
    }
    return true;
}

static int pg_incompatibility_previous_level(
    PgSolver *solver,
    PgIncompatibility *inc,
    PgPackageId skip_pkg
) {
    if (!solver || !inc) {
        return 1;
    }

    int level = 0;
    for (size_t i = 0; i < inc->term_count; i++) {
        PgTerm term = inc->terms[i];
        if (term.pkg == skip_pkg) {
            continue;
        }
        PgAssignment *assignment = pg_find_assignment_for_term(solver, term, NULL);
        if (assignment && assignment->decision_level > level) {
            level = assignment->decision_level;
        }
    }

    return level;
}

static bool pg_incompatibility_is_root_failure(
    PgSolver *solver,
    PgIncompatibility *inc
) {
    if (!solver || !inc) {
        return false;
    }

    if (inc->term_count == 0) {
        return true;
    }

    if (inc->term_count == 1) {
        PgTerm term = inc->terms[0];
        if (term.positive && term.pkg == solver->root_pkg) {
            return true;
        }
    }

    return false;
}

static PgIncompatibility *pg_incompatibility_resolve_with(
    PgSolver *solver,
    PgIncompatibility *left,
    PgIncompatibility *right,
    PgPackageId elim_pkg
) {
    if (!solver || !left || !right) {
        return NULL;
    }

    size_t new_term_count = 0;
    for (size_t i = 0; i < left->term_count; i++) {
        if (left->terms[i].pkg != elim_pkg) {
            new_term_count++;
        }
    }
    for (size_t i = 0; i < right->term_count; i++) {
        if (right->terms[i].pkg != elim_pkg) {
            new_term_count++;
        }
    }

    PgTerm *terms = NULL;
    if (new_term_count > 0) {
        terms = (PgTerm *)arena_malloc(new_term_count * sizeof(PgTerm));
        if (!terms) {
            return NULL;
        }
    }

    size_t idx = 0;
    for (size_t i = 0; i < left->term_count; i++) {
        if (left->terms[i].pkg == elim_pkg) {
            continue;
        }
        terms[idx++] = left->terms[i];
    }
    for (size_t i = 0; i < right->term_count; i++) {
        if (right->terms[i].pkg == elim_pkg) {
            continue;
        }
        terms[idx++] = right->terms[i];
    }

    PgIncompatibility *causes[2];
    causes[0] = left;
    causes[1] = right;

    PgIncompatibility *resolved = pg_incompatibility_new(
        terms,
        new_term_count,
        PG_REASON_INTERNAL,
        causes,
        2
    );

    arena_free(terms);

    if (!resolved) {
        return NULL;
    }

    if (!pg_solver_track_incompatibility(solver, resolved)) {
        pg_incompatibility_free(resolved);
        return NULL;
    }

    return resolved;
}

static PgIncompatibility *pg_resolve_conflict(
    PgSolver *solver,
    PgIncompatibility *conflict,
    int *out_backjump_level
) {
    if (!solver || !conflict || !out_backjump_level) {
        return NULL;
    }

    PgIncompatibility *current = conflict;

    int iteration = 0;
    while (true) {
        log_trace("resolve iteration %d, terms=%zu", iteration++, current->term_count);

        if (pg_incompatibility_is_root_failure(solver, current)) {
            log_trace("root failure detected");
            solver->root_incompatibility = current;
            return NULL;
        }

        size_t satisfier_index = SIZE_MAX;
        size_t term_index = SIZE_MAX;
        log_trace("finding satisfier");
        if (!pg_incompatibility_find_satisfier(
                solver,
                current,
                &satisfier_index,
                &term_index)) {
            log_trace("no satisfier found");
            return NULL;
        }
        log_trace("found satisfier at index %zu", satisfier_index);

        PgAssignment *satisfier = &solver->trail.items[satisfier_index];
        PgTerm term = current->terms[term_index];
        log_trace("satisfier pkg=%d, decided=%d, level=%d",
                satisfier->pkg, satisfier->decided, satisfier->decision_level);

        int previous_level = pg_incompatibility_previous_level(
            solver,
            current,
            satisfier->pkg
        );
        log_trace("previous_level=%d", previous_level);

        if (satisfier->decided ||
            previous_level == satisfier->decision_level) {
            log_trace("stopping resolution, backjump to %d", previous_level);
            /* If we would backjump to level 0 or below, it means the conflict
             * involves only root-level decisions (level 0 or 1). This indicates
             * that the root dependencies themselves are unsatisfiable.
             * Adjusting to level 1 would cause an infinite loop, so instead we
             * detect this as "no solution exists". */
            if (previous_level < 1) {
                log_trace("conflict at root level - no solution exists");
                solver->root_incompatibility = current;
                return NULL;
            }
            *out_backjump_level = previous_level;
            return current;
        }

        if (!satisfier->cause) {
            log_trace("satisfier has no cause (root decision or root dependency)");

            // When satisfier has no cause, it can be:
            // 1. The root package decision (level 1)
            // 2. A root dependency (level 0, added via add_root_dependency)
            // In both cases, we derive a final incompatibility by removing this term.
            if ((satisfier->pkg == solver->root_pkg && satisfier->decision_level == 1) ||
                satisfier->decision_level == 0) {
                log_trace("deriving final incompatibility (level=%d, pkg=%d)",
                        satisfier->decision_level, satisfier->pkg);

                // Create array of terms without the satisfier's term
                size_t new_term_count = current->term_count - 1;
                PgTerm *new_terms = NULL;

                if (new_term_count > 0) {
                    new_terms = (PgTerm *)arena_malloc(new_term_count * sizeof(PgTerm));
                    if (!new_terms) {
                        log_trace("failed to allocate terms array");
                        return NULL;
                    }

                    size_t write_idx = 0;
                    for (size_t i = 0; i < current->term_count; i++) {
                        if (i != term_index) {
                            new_terms[write_idx++] = current->terms[i];
                        }
                    }
                }

                // Create derived incompatibility with current as the only cause
                PgIncompatibility *causes[1] = {current};
                PgIncompatibility *derived = pg_incompatibility_new(
                    new_terms,
                    new_term_count,
                    PG_REASON_INTERNAL,
                    causes,
                    1
                );

                arena_free(new_terms);

                if (!derived) {
                    log_trace("failed to create derived incompatibility");
                    return NULL;
                }

                // This derived incompatibility is now the root cause
                solver->root_incompatibility = derived;
                log_trace("created root incompatibility with %zu terms", derived->term_count);
                return NULL;
            } else {
                log_trace("satisfier has no cause but is not root (pkg=%d, level=%d) - internal error",
                        satisfier->pkg, satisfier->decision_level);
                solver->root_incompatibility = current;
                return NULL;
            }
        }

        log_trace("resolving with cause");
        PgIncompatibility *resolved = pg_incompatibility_resolve_with(
            solver,
            current,
            satisfier->cause,
            term.pkg
        );
        if (!resolved) {
            log_trace("resolution failed");
            return NULL;
        }
        log_trace("resolved, new term_count=%zu", resolved->term_count);

        current = resolved;
    }
}

int pg_version_compare(PgVersion a, PgVersion b) {
    return pg_version_cmp_internal(&a, &b);
}

PgVersionRange pg_range_any(void) {
    PgVersionRange r;
    r.lower = pg_unbounded_bound();
    r.upper = pg_unbounded_bound();
    r.is_empty = false;
    return r;
}

PgVersionRange pg_range_exact(PgVersion v) {
    PgVersionRange r;
    r.lower.v = v;
    r.lower.inclusive = true;
    r.lower.unbounded = false;

    r.upper.v = v;
    r.upper.inclusive = true;
    r.upper.unbounded = false;

    r.is_empty = false;
    return r;
}

PgVersionRange pg_range_until_next_minor(PgVersion v) {
    PgVersionRange r;
    r.lower.v = v;
    r.lower.inclusive = true;
    r.lower.unbounded = false;

    r.upper.v.major = v.major;
    r.upper.v.minor = v.minor + 1;
    r.upper.v.patch = 0;
    r.upper.inclusive = false;
    r.upper.unbounded = false;

    r.is_empty = false;
    return r;
}

PgVersionRange pg_range_until_next_major(PgVersion v) {
    PgVersionRange r;
    r.lower.v = v;
    r.lower.inclusive = true;
    r.lower.unbounded = false;

    r.upper.v.major = v.major + 1;
    r.upper.v.minor = 0;
    r.upper.v.patch = 0;
    r.upper.inclusive = false;
    r.upper.unbounded = false;

    r.is_empty = false;
    return r;
}

PgVersionRange pg_range_intersect(PgVersionRange a, PgVersionRange b) {
    if (a.is_empty || b.is_empty) {
        PgVersionRange empty = pg_range_any();
        empty.is_empty = true;
        return empty;
    }

    PgVersionRange r = pg_range_any();

    /* Lower bound: pick the most restrictive (maximum) */
    if (a.lower.unbounded) {
        r.lower = b.lower;
    } else if (b.lower.unbounded) {
        r.lower = a.lower;
    } else {
        int cmp = pg_version_cmp_internal(&a.lower.v, &b.lower.v);
        if (cmp > 0) {
            r.lower = a.lower;
        } else if (cmp < 0) {
            r.lower = b.lower;
        } else {
            r.lower.v = a.lower.v;
            r.lower.unbounded = false;
            r.lower.inclusive = a.lower.inclusive && b.lower.inclusive;
        }
    }

    /* Upper bound: pick the most restrictive (minimum) */
    if (a.upper.unbounded) {
        r.upper = b.upper;
    } else if (b.upper.unbounded) {
        r.upper = a.upper;
    } else {
        int cmp = pg_version_cmp_internal(&a.upper.v, &b.upper.v);
        if (cmp < 0) {
            r.upper = a.upper;
        } else if (cmp > 0) {
            r.upper = b.upper;
        } else {
            r.upper.v = a.upper.v;
            r.upper.unbounded = false;
            r.upper.inclusive = a.upper.inclusive && b.upper.inclusive;
        }
    }

    /* Detect empty ranges */
    if (!r.lower.unbounded && !r.upper.unbounded) {
        int cmp = pg_version_cmp_internal(&r.lower.v, &r.upper.v);
        if (cmp > 0) {
            r.is_empty = true;
        } else if (cmp == 0 && (!r.lower.inclusive || !r.upper.inclusive)) {
            r.is_empty = true;
        } else {
            r.is_empty = false;
        }
    } else {
        r.is_empty = false;
    }

    return r;
}

bool pg_range_contains(PgVersionRange range, PgVersion v) {
    if (range.is_empty) {
        return false;
    }

    if (!range.lower.unbounded) {
        int cmp = pg_version_cmp_internal(&v, &range.lower.v);
        if (cmp < 0) {
            return false;
        }
        if (cmp == 0 && !range.lower.inclusive) {
            return false;
        }
    }

    if (!range.upper.unbounded) {
        int cmp = pg_version_cmp_internal(&v, &range.upper.v);
        if (cmp > 0) {
            return false;
        }
        if (cmp == 0 && !range.upper.inclusive) {
            return false;
        }
    }

    return true;
}

PgSolver *pg_solver_new(
    PgDependencyProvider provider,
    void *provider_ctx,
    PgPackageId root_pkg,
    PgVersion root_version
) {
    PgSolver *solver = (PgSolver *)arena_malloc(sizeof(PgSolver));
    if (!solver) {
        return NULL;
    }

    solver->provider = provider;
    solver->provider_ctx = provider_ctx;
    solver->root_pkg = root_pkg;
    solver->root_version = root_version;

    pg_trail_init(&solver->trail);
    solver->pkg_states = NULL;
    solver->pkg_state_capacity = 0;
    solver->pkg_incompat_lists = NULL;
    solver->pkg_incompat_capacity = 0;
    solver->incompatibilities = NULL;
    solver->incompatibility_count = 0;
    solver->incompatibility_capacity = 0;
    solver->current_decision_level = 0;
    solver->changed_pkgs = NULL;
    solver->changed_count = 0;
    solver->changed_capacity = 0;
    solver->solved = false;
    solver->root_incompatibility = NULL;

    /* Initialize version cache */
    solver->cached_versions = NULL;
    solver->cached_counts = NULL;
    solver->cache_capacity = 0;

    /* Initialize statistics */
    solver->stats_cache_hits = 0;
    solver->stats_cache_misses = 0;
    solver->stats_decisions = 0;
    solver->stats_propagations = 0;
    solver->stats_conflicts = 0;

    return solver;
}

void pg_solver_free(PgSolver *solver) {
    if (!solver) {
        return;
    }

    /* Free incompatibilities */
    if (solver->incompatibilities) {
        for (size_t i = 0; i < solver->incompatibility_count; i++) {
            pg_incompatibility_free(solver->incompatibilities[i]);
        }
        arena_free(solver->incompatibilities);
    }

    pg_trail_free(&solver->trail);
    arena_free(solver->pkg_states);
    if (solver->pkg_incompat_lists) {
        for (int i = 0; i < solver->pkg_incompat_capacity; i++) {
            arena_free(solver->pkg_incompat_lists[i].items);
        }
        arena_free(solver->pkg_incompat_lists);
    }
    arena_free(solver->changed_pkgs);

    /* Free version cache */
    if (solver->cached_versions) {
        for (int i = 0; i < solver->cache_capacity; i++) {
            arena_free(solver->cached_versions[i]);
        }
        arena_free(solver->cached_versions);
    }
    arena_free(solver->cached_counts);

    arena_free(solver);
}

bool pg_solver_add_root_dependency(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersionRange range
) {
    /* Root dependencies are modeled as positive requirements on packages.
     * They are not decisions in the PubGrub sense, so we record them as
     * derived assignments at decision level 0 with no cause.
     */
    return pg_solver_add_positive_requirement(
        solver,
        pkg,
        range,
        false,
        0,
        NULL
    );
}

/* Temporary solving loop.
 *
 * The solver alternates between unit propagation and a simple decision
 * heuristic (pick the first package with an outstanding positive range and
 * choose its newest allowed version). Conflicts from either propagation or
 * failed decisions are resolved via PubGrub’s clause learning/backjumping.
 */
PgSolverStatus pg_solver_solve(PgSolver *solver) {
    if (!solver) {
        return PG_SOLVER_INTERNAL_ERROR;
    }

    /* If we already solved once, do not re-run. */
    if (solver->solved) {
        return PG_SOLVER_OK;
    }

    /* Seed the trail with a decision for the root package. */
    PgAssignment root_assignment;
    root_assignment.pkg = solver->root_pkg;
    root_assignment.range = pg_range_exact(solver->root_version);
    root_assignment.positive = true;
    root_assignment.decided = true;
    root_assignment.decision_level = 1;
    root_assignment.cause = NULL;

    if (!pg_solver_push_assignment(solver, root_assignment)) {
        return PG_SOLVER_INTERNAL_ERROR;
    }

    PgSolverStatus dep_status = pg_solver_register_dependencies(
        solver,
        solver->root_pkg,
        root_assignment.range,
        solver->root_version
    );
    if (dep_status != PG_SOLVER_OK) {
        return dep_status;
    }

    solver->current_decision_level = 1;

    while (true) {
        PgIncompatibility *conflict = NULL;
        PgSolverStatus status = pg_unit_propagate(solver, &conflict);
        if (status != PG_SOLVER_OK) {
            return status;
        }
        solver->stats_propagations++;
        log_trace("propagation done conflict=%p", (void *)conflict);

        if (!conflict) {
            bool made_decision = false;
            status = pg_make_decision(solver, &made_decision, &conflict);
            if (status != PG_SOLVER_OK) {
                return status;
            }

            if (!conflict && !made_decision) {
                log_trace("solve complete");
                break;
            }

            if (!conflict) {
                solver->stats_decisions++;
                log_trace("made decision");
                continue;
            }
        }

        solver->stats_conflicts++;
        log_trace("resolving conflict");
        int backjump_level = 0;
        PgIncompatibility *learned = pg_resolve_conflict(
            solver,
            conflict,
            &backjump_level
        );
        if (!learned) {
            log_trace("no solution found");
            return PG_SOLVER_NO_SOLUTION;
        }
        log_trace("learned incompatibility with %zu terms, backjump to %d",
                learned->term_count, backjump_level);

        if (!learned->attached) {
            log_trace("attaching learned incompatibility");
            if (!pg_solver_attach_incompatibility(solver, learned)) {
                log_trace("attach failed");
                return PG_SOLVER_INTERNAL_ERROR;
            }
            log_trace("attached");
        }

        log_trace("backtracking to level %d", backjump_level);
        if (!pg_solver_backtrack_to_level(solver, backjump_level)) {
            log_trace("backtrack failed");
            return PG_SOLVER_INTERNAL_ERROR;
        }
        log_trace("backtracked, continuing main loop");
    }

    solver->solved = true;
    return PG_SOLVER_OK;
}

bool pg_solver_get_selected_version(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersion *out_version
) {
    if (!solver || !out_version) {
        return false;
    }

    /* Scan the trail for a positive assignment for this package. */
    for (size_t i = 0; i < solver->trail.count; i++) {
        PgAssignment *a = &solver->trail.items[i];
        if (a->pkg != pkg || !a->positive) {
            continue;
        }

        /* For now we only support exact ranges here. */
        if (a->range.lower.unbounded || a->range.upper.unbounded) {
            continue;
        }

        if (pg_version_cmp_internal(&a->range.lower.v, &a->range.upper.v) == 0 &&
            a->range.lower.inclusive &&
            a->range.upper.inclusive) {
            *out_version = a->range.lower.v;
            return true;
        }
    }

    return false;
}

void pg_solver_get_stats(PgSolver *solver, PgSolverStats *out_stats) {
    if (!solver || !out_stats) {
        return;
    }

    out_stats->cache_hits = solver->stats_cache_hits;
    out_stats->cache_misses = solver->stats_cache_misses;
    out_stats->decisions = solver->stats_decisions;
    out_stats->propagations = solver->stats_propagations;
    out_stats->conflicts = solver->stats_conflicts;
}

/* Error reporting implementation */

typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t offset;
    bool truncated;
} PgErrorWriter;

static void pg_error_writer_init(PgErrorWriter *w, char *buffer, size_t buffer_size) {
    w->buffer = buffer;
    w->buffer_size = buffer_size;
    w->offset = 0;
    w->truncated = false;
    if (buffer && buffer_size > 0) {
        buffer[0] = '\0';
    }
}

static void pg_error_writer_append(PgErrorWriter *w, const char *text) {
    if (!w || !text || w->truncated) {
        return;
    }

    size_t text_len = strlen(text);
    if (w->offset + text_len + 1 > w->buffer_size) {
        w->truncated = true;
        return;
    }

    memcpy(w->buffer + w->offset, text, text_len);
    w->offset += text_len;
    w->buffer[w->offset] = '\0';
}

static void pg_error_writer_appendf(PgErrorWriter *w, const char *fmt, ...) {
    if (!w || w->truncated) {
        return;
    }

    char temp[MAX_TEMP_PATH_LENGTH];
    va_list args;
    va_start(args, fmt);
    vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    pg_error_writer_append(w, temp);
}

/* Line numbering for complex error messages */
typedef struct {
    PgIncompatibility **incompatibilities;
    int *line_numbers;
    size_t count;
    size_t capacity;
    int next_line_number;
} PgLineNumbering;

static void pg_line_numbering_init(PgLineNumbering *ln) {
    ln->incompatibilities = NULL;
    ln->line_numbers = NULL;
    ln->count = 0;
    ln->capacity = 0;
    ln->next_line_number = 1;
}

static void pg_line_numbering_free(PgLineNumbering *ln) {
    arena_free(ln->incompatibilities);
    arena_free(ln->line_numbers);
}

static int pg_line_numbering_get(PgLineNumbering *ln, PgIncompatibility *inc) {
    for (size_t i = 0; i < ln->count; i++) {
        if (ln->incompatibilities[i] == inc) {
            return ln->line_numbers[i];
        }
    }
    return 0;
}

static bool pg_line_numbering_assign(PgLineNumbering *ln, PgIncompatibility *inc) {
    /* Check if already assigned */
    if (pg_line_numbering_get(ln, inc) != 0) {
        return true;
    }

    /* Expand capacity if needed */
    if (ln->count >= ln->capacity) {
        size_t new_capacity = ln->capacity == 0 ? 16 : ln->capacity * 2;
        PgIncompatibility **new_incs = arena_realloc(
            ln->incompatibilities,
            new_capacity * sizeof(PgIncompatibility *)
        );
        int *new_lines = arena_realloc(
            ln->line_numbers,
            new_capacity * sizeof(int)
        );
        if (!new_incs || !new_lines) {
            arena_free(new_incs);
            arena_free(new_lines);
            return false;
        }
        ln->incompatibilities = new_incs;
        ln->line_numbers = new_lines;
        ln->capacity = new_capacity;
    }

    ln->incompatibilities[ln->count] = inc;
    ln->line_numbers[ln->count] = ln->next_line_number;
    ln->count++;
    ln->next_line_number++;

    return true;
}

static void pg_format_version_range(
    PgVersionRange range,
    char *out,
    size_t out_size,
    bool *out_is_any
) {
    /* Check if it's "any" (unbounded on both ends) */
    if (range.lower.unbounded && range.upper.unbounded) {
        snprintf(out, out_size, "any");
        if (out_is_any) *out_is_any = true;
        return;
    }

    if (out_is_any) *out_is_any = false;

    /* Check if it's an exact version */
    if (!range.lower.unbounded && !range.upper.unbounded &&
        range.lower.inclusive && range.upper.inclusive &&
        pg_version_cmp_internal(&range.lower.v, &range.upper.v) == 0) {
        snprintf(out, out_size, "%d.%d.%d",
                 range.lower.v.major,
                 range.lower.v.minor,
                 range.lower.v.patch);
        return;
    }

    /* Check for caret ranges (^X.Y.Z = >=X.Y.Z <X+1.0.0) */
    if (!range.lower.unbounded && !range.upper.unbounded &&
        range.lower.inclusive && !range.upper.inclusive &&
        range.upper.v.minor == 0 && range.upper.v.patch == 0 &&
        range.upper.v.major == range.lower.v.major + 1) {
        snprintf(out, out_size, "^%d.%d.%d",
                 range.lower.v.major,
                 range.lower.v.minor,
                 range.lower.v.patch);
        return;
    }

    /* Generic range */
    char lower_str[MAX_VERSION_STRING_MEDIUM_LENGTH] = "";
    char upper_str[MAX_VERSION_STRING_MEDIUM_LENGTH] = "";

    if (!range.lower.unbounded) {
        snprintf(lower_str, sizeof(lower_str), "%s%d.%d.%d",
                 range.lower.inclusive ? ">=" : ">",
                 range.lower.v.major,
                 range.lower.v.minor,
                 range.lower.v.patch);
    }

    if (!range.upper.unbounded) {
        snprintf(upper_str, sizeof(upper_str), "%s%d.%d.%d",
                 range.upper.inclusive ? "<=" : "<",
                 range.upper.v.major,
                 range.upper.v.minor,
                 range.upper.v.patch);
    }

    if (lower_str[0] && upper_str[0]) {
        snprintf(out, out_size, "%s %s", lower_str, upper_str);
    } else if (lower_str[0]) {
        snprintf(out, out_size, "%s", lower_str);
    } else if (upper_str[0]) {
        snprintf(out, out_size, "%s", upper_str);
    } else {
        snprintf(out, out_size, "any");
    }
}

static void pg_format_term(
    PgTerm *term,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    char *out,
    size_t out_size
) {
    const char *pkg_name = name_resolver(name_ctx, term->pkg);
    char range_str[MAX_RANGE_STRING_LENGTH];
    bool is_any;
    pg_format_version_range(term->range, range_str, sizeof(range_str), &is_any);

    if (is_any && term->positive) {
        snprintf(out, out_size, "%s", pkg_name);
    } else if (is_any && !term->positive) {
        snprintf(out, out_size, "not %s", pkg_name);
    } else if (term->positive) {
        snprintf(out, out_size, "%s %s", pkg_name, range_str);
    } else {
        snprintf(out, out_size, "not %s %s", pkg_name, range_str);
    }
}

/* Forward declaration */
static void pg_explain_incompatibility(
    PgIncompatibility *inc,
    PgErrorWriter *writer,
    PgLineNumbering *ln,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    PgPackageId root_pkg,
    bool is_root
);

/* Count how many incompatibilities are caused by this one (heuristic) */
static int pg_count_outgoing_edges(PgIncompatibility *inc) {
    /* Simplified heuristic: derived incompatibilities with multiple causes
     * are more likely to be referenced later */
    if (inc->cause_count >= 2) {
        return 2;
    }
    return 1;
}

/* Check if incompatibility is "simple" (caused by two external incompatibilities) */
static bool pg_is_simple_derived(PgIncompatibility *inc) {
    return inc->cause_count == 2 &&
           inc->causes[0]->cause_count == 0 &&
           inc->causes[1]->cause_count == 0;
}

/* Explain a dependency incompatibility inline */
static void pg_explain_dependency_inline(
    PgIncompatibility *inc,
    PgErrorWriter *writer,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    PgPackageId root_pkg
) {
    if (inc->term_count != 2) {
        pg_error_writer_append(writer, "[malformed dependency]");
        return;
    }

    char pkg_str[MAX_TERM_STRING_LENGTH];
    char dep_str[MAX_TERM_STRING_LENGTH];

    PgTerm depender = inc->terms[0];
    PgTerm dependency = inc->terms[1];

    /* Format depender */
    const char *depender_name = name_resolver(name_ctx, depender.pkg);
    char depender_range[MAX_RANGE_STRING_LENGTH];
    bool depender_is_any;
    pg_format_version_range(depender.range, depender_range, sizeof(depender_range), &depender_is_any);

    /* Special formatting for root package */
    if (depender.pkg == root_pkg) {
        if (depender_is_any) {
            snprintf(pkg_str, sizeof(pkg_str), "your app");
        } else {
            snprintf(pkg_str, sizeof(pkg_str), "your app (%s)", depender_range);
        }
    } else {
        if (depender_is_any) {
            snprintf(pkg_str, sizeof(pkg_str), "%s", depender_name);
        } else {
            snprintf(pkg_str, sizeof(pkg_str), "%s %s", depender_name, depender_range);
        }
    }

    /* Format dependency (flip negative to positive) */
    dependency.positive = !dependency.positive;
    pg_format_term(&dependency, name_resolver, name_ctx, dep_str, sizeof(dep_str));

    pg_error_writer_appendf(writer, "%s depends on %s", pkg_str, dep_str);
}

/* Explain an external incompatibility inline (e.g., NO_VERSIONS) */
static void pg_explain_external_inline(
    PgIncompatibility *inc,
    PgErrorWriter *writer,
    PgPackageNameResolver name_resolver,
    void *name_ctx
) {
    if (!inc) {
        pg_error_writer_append(writer, "[external constraint]");
        return;
    }

    if (inc->reason == PG_REASON_NO_VERSIONS && inc->term_count > 0) {
        char term_str[MAX_TERM_STRING_LENGTH];
        PgTerm term = inc->terms[0];
        if (!term.positive) {
            term.positive = true;
        }
        pg_format_term(&term, name_resolver, name_ctx, term_str, sizeof(term_str));

        /* Try to show the required range and current pinned version (if available) */
        char range_str[MAX_RANGE_STRING_LENGTH];
        bool is_any = false;
        pg_format_version_range(term.range, range_str, sizeof(range_str), &is_any);

        const char *current_version = NULL;
        const char *pkg_name = name_resolver ? name_resolver(name_ctx, term.pkg) : NULL;
        if (name_ctx && pkg_name) {
            PgExplainContext *ctx = (PgExplainContext *)name_ctx;
            if (ctx->current_packages) {
                const char *slash = strchr(pkg_name, '/');
                if (slash) {
                    size_t author_len = (size_t)(slash - pkg_name);
                    char *author = arena_malloc(author_len + 1);
                    if (author) {
                        memcpy(author, pkg_name, author_len);
                        author[author_len] = '\0';
                        const char *name = slash + 1;
                        Package *pkg = package_map_find(ctx->current_packages, author, name);
                        if (pkg) {
                            current_version = pkg->version;
                        }
                        arena_free(author);
                    }
                }
            }
        }

        if (current_version) {
            pg_error_writer_appendf(writer,
                "no versions of %s satisfy the constraints (%s) while your project pins %s",
                term_str, is_any ? "any version" : range_str, current_version);
        } else {
            pg_error_writer_appendf(writer,
                "no versions of %s satisfy the constraints%s%s",
                term_str,
                is_any ? "" : " ",
                is_any ? "" : range_str);
        }
        return;
    }

    pg_error_writer_append(writer, "[external constraint]");
}

/* Explain the conclusion derived from an incompatibility */
static void pg_explain_conclusion(
    PgIncompatibility *inc,
    PgErrorWriter *writer,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    PgPackageId root_pkg
) {
    /* Handle special case: root package incompatibility (no solution) */
    if (inc->term_count == 1 && inc->terms[0].pkg == root_pkg && inc->terms[0].positive) {
        pg_error_writer_append(writer, "version solving failed");
        return;
    }

    /* For other derived incompatibilities, format as "X requires Y" or "X is forbidden" */
    if (inc->term_count == 1) {
        char term_str[MAX_TERM_STRING_LENGTH];
        /* Flip to show what's required/forbidden */
        PgTerm flipped = inc->terms[0];
        flipped.positive = !flipped.positive;

        if (flipped.pkg == root_pkg) {
            pg_error_writer_append(writer, "version solving failed");
        } else {
            pg_format_term(&flipped, name_resolver, name_ctx, term_str, sizeof(term_str));
            if (flipped.positive) {
                pg_error_writer_appendf(writer, "%s is forbidden", term_str);
            } else {
                pg_error_writer_appendf(writer, "%s is required", term_str);
            }
        }
        return;
    }

    /* For multi-term conclusions */
    if (inc->term_count == 2) {
        char term1_str[MAX_TERM_STRING_LENGTH];
        char term2_str[MAX_TERM_STRING_LENGTH];

        PgTerm t1 = inc->terms[0];
        PgTerm t2 = inc->terms[1];

        /* Both positive: "X and Y are incompatible" */
        /* One positive, one negative: "X requires Y" */
        if (t1.positive && t2.positive) {
            pg_format_term(&t1, name_resolver, name_ctx, term1_str, sizeof(term1_str));
            pg_format_term(&t2, name_resolver, name_ctx, term2_str, sizeof(term2_str));
            pg_error_writer_appendf(writer, "%s and %s are incompatible", term1_str, term2_str);
        } else {
            /* Find the positive term (what's incompatible) and negative term (what's required) */
            PgTerm positive_term = t1.positive ? t1 : t2;
            PgTerm negative_term = t1.positive ? t2 : t1;
            negative_term.positive = !negative_term.positive;

            pg_format_term(&positive_term, name_resolver, name_ctx, term1_str, sizeof(term1_str));
            pg_format_term(&negative_term, name_resolver, name_ctx, term2_str, sizeof(term2_str));
            pg_error_writer_appendf(writer, "%s requires %s", term1_str, term2_str);
        }
        return;
    }

    /* Fallback for complex cases */
    pg_error_writer_append(writer, "the constraints are incompatible");
}

/*
 * Main recursive explanation function implementing the PubGrub error reporting algorithm.
 * Follows the spec from doc/pubgrub-solver.md section "Error Reporting".
 */
static void pg_explain_incompatibility(
    PgIncompatibility *inc,
    PgErrorWriter *writer,
    PgLineNumbering *ln,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    PgPackageId root_pkg,
    bool is_root __attribute__((unused))
) {
    if (!inc || !writer) {
        return;
    }

    /* Special case: Empty incompatibility (0 terms) - direct contradiction */
    /* This happens when we've derived that no solution exists */
    if (inc->term_count == 0) {
        if (inc->cause_count == 1) {
            /* Explain the single cause that led to this contradiction */
            pg_explain_incompatibility(inc->causes[0], writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_error_writer_append(writer, "Thus, version solving failed.\n");
        } else if (inc->cause_count == 2) {
            /* Two causes led to contradiction */
            pg_explain_incompatibility(inc->causes[0], writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_explain_incompatibility(inc->causes[1], writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_error_writer_append(writer, "Thus, version solving failed.\n");
        } else {
            pg_error_writer_append(writer, "Version solving failed.\n");
        }
        return;
    }

    /* Case 1: Both external (base case) */
    if (inc->cause_count == 2 &&
        inc->causes[0]->cause_count == 0 &&
        inc->causes[1]->cause_count == 0) {

        /* Special case: root depends on package with no versions.
         * We used to short-circuit with a "package does not exist" message here,
         * but that hides conflicts when the package actually exists in the registry.
         * Continue to the generic explanation so we show which constraints block it. */
        PgIncompatibility *dep_cause = NULL;
        PgIncompatibility *no_vers_cause = NULL;

        if (inc->causes[0]->reason == PG_REASON_DEPENDENCY &&
            inc->causes[1]->reason == PG_REASON_NO_VERSIONS) {
            dep_cause = inc->causes[0];
            no_vers_cause = inc->causes[1];
        } else if (inc->causes[1]->reason == PG_REASON_DEPENDENCY &&
                   inc->causes[0]->reason == PG_REASON_NO_VERSIONS) {
            dep_cause = inc->causes[1];
            no_vers_cause = inc->causes[0];
        }

        pg_error_writer_append(writer, "Because ");
        if (inc->causes[0]->reason == PG_REASON_DEPENDENCY) {
            pg_explain_dependency_inline(inc->causes[0], writer, name_resolver, name_ctx, root_pkg);
        } else {
            pg_explain_external_inline(inc->causes[0], writer, name_resolver, name_ctx);
        }
        pg_error_writer_append(writer, " and ");
        if (inc->causes[1]->reason == PG_REASON_DEPENDENCY) {
            pg_explain_dependency_inline(inc->causes[1], writer, name_resolver, name_ctx, root_pkg);
        } else {
            pg_explain_external_inline(inc->causes[1], writer, name_resolver, name_ctx);
        }
        pg_error_writer_append(writer, ", ");
        pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
        pg_error_writer_append(writer, ".\n");
        (void)dep_cause;
        (void)no_vers_cause;
        return;
    }

    /* Special case: Single positive root term - root package can't be satisfied */
    if (inc->term_count == 1 && inc->terms[0].pkg == root_pkg && inc->terms[0].positive) {
        if (inc->cause_count > 0) {
            /* Check for the common pattern: trying to install a non-existent package
             * where one cause is NO_VERSIONS and the other is a DEPENDENCY from root.
             * Only emit the shortcut when the NO_VERSIONS cause is external (no derived causes),
             * otherwise explain the real conflict chain. */
            if (inc->cause_count == 2) {
                PgIncompatibility *dep_cause = NULL;
                PgIncompatibility *no_vers_cause = NULL;

                if (inc->causes[0]->reason == PG_REASON_DEPENDENCY &&
                    inc->causes[1]->reason == PG_REASON_NO_VERSIONS) {
                    dep_cause = inc->causes[0];
                    no_vers_cause = inc->causes[1];
                } else if (inc->causes[1]->reason == PG_REASON_DEPENDENCY &&
                           inc->causes[0]->reason == PG_REASON_NO_VERSIONS) {
                    dep_cause = inc->causes[1];
                    no_vers_cause = inc->causes[0];
                }

                /* If we have a dependency from root to a package with no versions,
                 * we used to emit a "does not exist" shortcut. Keep going instead so
                 * we surface the actual constraints that removed all versions. */
                if (dep_cause && no_vers_cause &&
                    dep_cause->term_count == 2 &&
                    no_vers_cause->term_count > 0 &&
                    no_vers_cause->cause_count == 0) {
                    PgTerm depender = dep_cause->terms[0];
                    PgTerm dependency = dep_cause->terms[1];
                    PgTerm no_vers_term = no_vers_cause->terms[0];

                    (void)depender;
                    (void)dependency;
                    (void)no_vers_term;
                }
            }

            /* Default: explain why the root package can't be satisfied */
            pg_explain_incompatibility(inc->causes[0], writer, ln, name_resolver, name_ctx, root_pkg, false);
            if (inc->cause_count > 1) {
                pg_explain_incompatibility(inc->causes[1], writer, ln, name_resolver, name_ctx, root_pkg, false);
            }
            pg_error_writer_append(writer, "So, version solving failed.\n");
        } else {
            pg_error_writer_append(writer, "Version solving failed.\n");
        }
        return;
    }

    /* Case 2: Two derived causes */
    if (inc->cause_count == 2 &&
        inc->causes[0]->cause_count > 0 &&
        inc->causes[1]->cause_count > 0) {

        int line1 = pg_line_numbering_get(ln, inc->causes[0]);
        int line2 = pg_line_numbering_get(ln, inc->causes[1]);

        /* Case 1.1: Both already have line numbers */
        if (line1 > 0 && line2 > 0) {
            pg_error_writer_appendf(writer, "Because (%d) and (%d), ", line1, line2);
            pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
            pg_error_writer_append(writer, ".\n");
            return;
        }

        /* Case 1.2: Only one has a line number */
        if (line1 > 0 || line2 > 0) {
            PgIncompatibility *unnumbered = (line1 > 0) ? inc->causes[1] : inc->causes[0];
            int line = (line1 > 0) ? line1 : line2;

            pg_explain_incompatibility(unnumbered, writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_error_writer_appendf(writer, "And because (%d), ", line);
            pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
            pg_error_writer_append(writer, ".\n");
            return;
        }

        /* Case 1.3: Neither has a line number */
        /* Check if one is simpler (both external causes) */
        bool cause0_simple = pg_is_simple_derived(inc->causes[0]);
        bool cause1_simple = pg_is_simple_derived(inc->causes[1]);

        if (cause0_simple || cause1_simple) {
            PgIncompatibility *simple = cause0_simple ? inc->causes[0] : inc->causes[1];
            PgIncompatibility *complex = cause0_simple ? inc->causes[1] : inc->causes[0];

            pg_explain_incompatibility(complex, writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_explain_incompatibility(simple, writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_error_writer_append(writer, "Thus, ");
            pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
            pg_error_writer_append(writer, ".\n");
            return;
        }

        /* Both complex: number them */
        pg_explain_incompatibility(inc->causes[0], writer, ln, name_resolver, name_ctx, root_pkg, false);
        if (pg_count_outgoing_edges(inc->causes[0]) > 1) {
            pg_line_numbering_assign(ln, inc->causes[0]);
        }

        pg_error_writer_append(writer, "\n");

        pg_explain_incompatibility(inc->causes[1], writer, ln, name_resolver, name_ctx, root_pkg, false);
        pg_line_numbering_assign(ln, inc->causes[1]);

        int assigned_line = pg_line_numbering_get(ln, inc->causes[1]);
        pg_error_writer_appendf(writer, "And because (%d), ", assigned_line);
        pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
        pg_error_writer_append(writer, ".\n");
        return;
    }

    /* Case 2: One derived, one external */
    if (inc->cause_count == 2) {
        PgIncompatibility *derived = NULL;
        PgIncompatibility *external = NULL;

        if (inc->causes[0]->cause_count > 0) {
            derived = inc->causes[0];
            external = inc->causes[1];
        } else if (inc->causes[1]->cause_count > 0) {
            derived = inc->causes[1];
            external = inc->causes[0];
        }

        if (derived && external) {
            int derived_line = pg_line_numbering_get(ln, derived);

            /* Case 2.1: Derived already has line number */
            if (derived_line > 0) {
                pg_error_writer_append(writer, "Because ");
                if (external->reason == PG_REASON_DEPENDENCY) {
                    pg_explain_dependency_inline(external, writer, name_resolver, name_ctx, root_pkg);
                } else {
                    pg_explain_external_inline(external, writer, name_resolver, name_ctx);
                }
                pg_error_writer_appendf(writer, " and (%d), ", derived_line);
                pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
                pg_error_writer_append(writer, ".\n");
                return;
            }

            /* Case 2.2: Derived has one derived cause with no line number */
            if (derived->cause_count == 2) {
                PgIncompatibility *prior_derived = NULL;
                PgIncompatibility *prior_external = NULL;

                if (derived->causes[0]->cause_count > 0 &&
                    pg_line_numbering_get(ln, derived->causes[0]) == 0) {
                    prior_derived = derived->causes[0];
                    prior_external = derived->causes[1];
                } else if (derived->causes[1]->cause_count > 0 &&
                           pg_line_numbering_get(ln, derived->causes[1]) == 0) {
                    prior_derived = derived->causes[1];
                    prior_external = derived->causes[0];
                }

                if (prior_derived && prior_external && prior_external->cause_count == 0) {
                    pg_explain_incompatibility(prior_derived, writer, ln, name_resolver, name_ctx, root_pkg, false);
                    pg_error_writer_append(writer, "And because ");
                    if (prior_external->reason == PG_REASON_DEPENDENCY) {
                        pg_explain_dependency_inline(prior_external, writer, name_resolver, name_ctx, root_pkg);
                    } else {
                        pg_explain_external_inline(prior_external, writer, name_resolver, name_ctx);
                    }
                    pg_error_writer_append(writer, " and ");
                    if (external->reason == PG_REASON_DEPENDENCY) {
                        pg_explain_dependency_inline(external, writer, name_resolver, name_ctx, root_pkg);
                    } else {
                        pg_explain_external_inline(external, writer, name_resolver, name_ctx);
                    }
                    pg_error_writer_append(writer, ", ");
                    pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
                    pg_error_writer_append(writer, ".\n");
                    return;
                }
            }

            /* Case 2.3: General case */
            pg_explain_incompatibility(derived, writer, ln, name_resolver, name_ctx, root_pkg, false);
            pg_error_writer_append(writer, "And because ");
            if (external->reason == PG_REASON_DEPENDENCY) {
                pg_explain_dependency_inline(external, writer, name_resolver, name_ctx, root_pkg);
            } else {
                pg_explain_external_inline(external, writer, name_resolver, name_ctx);
            }
            pg_error_writer_append(writer, ", ");
            pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
            pg_error_writer_append(writer, ".\n");
            return;
        }
    }

    /* Case 3: Both external (base case) */
    if (inc->cause_count == 2 &&
        inc->causes[0]->cause_count == 0 &&
        inc->causes[1]->cause_count == 0) {

        pg_error_writer_append(writer, "Because ");
        if (inc->causes[0]->reason == PG_REASON_DEPENDENCY) {
            pg_explain_dependency_inline(inc->causes[0], writer, name_resolver, name_ctx, root_pkg);
        } else {
            pg_explain_external_inline(inc->causes[0], writer, name_resolver, name_ctx);
        }
        pg_error_writer_append(writer, " and ");
        if (inc->causes[1]->reason == PG_REASON_DEPENDENCY) {
            pg_explain_dependency_inline(inc->causes[1], writer, name_resolver, name_ctx, root_pkg);
        } else {
            pg_explain_external_inline(inc->causes[1], writer, name_resolver, name_ctx);
        }
        pg_error_writer_append(writer, ", ");
        pg_explain_conclusion(inc, writer, name_resolver, name_ctx, root_pkg);
        pg_error_writer_append(writer, ".\n");
        return;
    }

    /* Fallback: single cause or external incompatibility */
    if (inc->reason == PG_REASON_DEPENDENCY) {
        pg_explain_dependency_inline(inc, writer, name_resolver, name_ctx, root_pkg);
        pg_error_writer_append(writer, ".\n");
        return;
    }

    if (inc->reason == PG_REASON_NO_VERSIONS) {
        if (inc->term_count > 0) {
            char term_str[MAX_TERM_STRING_LENGTH];
            PgTerm term = inc->terms[0];
            /* Flip negative to positive for display */
            if (!term.positive) {
                term.positive = true;
            }
            pg_format_term(&term, name_resolver, name_ctx, term_str, sizeof(term_str));

            /* Check if it's the root package */
            if (term.pkg == root_pkg) {
                pg_error_writer_append(writer, "Your app's dependencies are incompatible");
            } else {
                pg_error_writer_appendf(writer, "No versions of %s satisfy the constraints", term_str);
            }
        } else {
            pg_error_writer_append(writer, "No compatible versions available");
        }
        pg_error_writer_append(writer, ".\n");
        return;
    }

    pg_error_writer_append(writer, "[incompatibility].\n");
}

bool pg_solver_explain_failure(
    PgSolver *solver,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    char *out_buffer,
    size_t buffer_size
) {
    if (!solver || !name_resolver || !out_buffer || buffer_size == 0) {
        return false;
    }

    if (!solver->root_incompatibility) {
        snprintf(out_buffer, buffer_size, "No error information available.");
        return true;
    }

    PgErrorWriter writer;
    pg_error_writer_init(&writer, out_buffer, buffer_size);

    PgLineNumbering ln;
    pg_line_numbering_init(&ln);

    /* Check if this is a proper derived incompatibility */
    if (solver->root_incompatibility->cause_count > 0) {
        /* Generate narrative explanation following PubGrub spec */
        pg_explain_incompatibility(
            solver->root_incompatibility,
            &writer,
            &ln,
            name_resolver,
            name_ctx,
            solver->root_pkg,
            true
        );
    } else {
        /* Simple external incompatibility - shouldn't normally happen for root */
        pg_error_writer_append(&writer, "Version solving failed.\n\n");

        if (solver->root_incompatibility->reason == PG_REASON_NO_VERSIONS) {
            if (solver->root_incompatibility->term_count > 0) {
                char term_str[MAX_TERM_STRING_LENGTH];
                PgTerm term = solver->root_incompatibility->terms[0];
                pg_format_term(&term, name_resolver, name_ctx, term_str, sizeof(term_str));
                pg_error_writer_appendf(&writer, "No versions of %s satisfy the constraints.\n", term_str);
            }
        } else if (solver->root_incompatibility->reason == PG_REASON_DEPENDENCY) {
            pg_explain_dependency_inline(solver->root_incompatibility, &writer, name_resolver, name_ctx, solver->root_pkg);
            pg_error_writer_append(&writer, ".\n");
        }
    }

    pg_line_numbering_free(&ln);

    return !writer.truncated;
}
