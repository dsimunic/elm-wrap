#ifndef RULR_REL_PROVIDER_H
#define RULR_REL_PROVIDER_H

/*
 * Relation Provider Interface - True BYODS Support
 *
 * This module implements the core insight from the BYODS paper:
 * specialized data structures that BACK Datalog relations, providing
 * algorithmic speedups by implicitly representing tuples.
 *
 * Key difference from ByodsProvider (external fact providers):
 * - ByodsProvider: iterates over external data (no algorithmic improvement)
 * - RelProvider: specialized storage that can represent N^2 tuples with O(N) space
 *
 * Example: eqrel (equivalence relation) backed by union-find
 * - add(a, b)      -> uf_union(a, b)
 * - contains(a, b) -> uf_find(a) == uf_find(b)
 * - No explicit closure rules needed
 * - O(N) storage instead of O(N^2)
 * - O(N*alpha(N)) time instead of O(N^3)
 *
 * See doc/byods-support-rearchitecture.md for full design.
 * See doc/papers/byods-paper.md for paper summary.
 */

#include "common/types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Provider kinds - each has different algorithmic properties.
 */
typedef enum {
    /* Standard tuple storage (hash set). Default for all relations. */
    REL_PROVIDER_EXPLICIT,

    /* Union-find backed equivalence relation.
     * Automatically computes reflexive, symmetric, transitive closure.
     * O(N) storage, O(N*alpha(N)) for N equivalences. */
    REL_PROVIDER_EQREL,

    /* E-graph backed provider with congruence closure.
     * Combines union-find with function application tracking.
     * If A=B, then f(A)=f(B) is automatically derived. */
    REL_PROVIDER_EGRAPH,

    /* Future: transitive relation with SCC compression */
    REL_PROVIDER_TRREL,

    /* Future: lattice-valued relation (Flix-style) */
    REL_PROVIDER_LATTICE
} RelProviderKind;

/*
 * Callback for emitting tuples during iteration.
 * Same signature as ByodsEmitFn for compatibility.
 *
 * @param fields  Array of Values representing the tuple
 * @param arity   Number of fields
 * @param user    User data passed through from iteration call
 * @return        0 to continue, non-zero to stop early
 */
typedef int (*RelEmitFn)(const Value *fields, int arity, void *user);

/*
 * Result codes for provider operations.
 */
typedef enum {
    REL_OK = 0,           /* Operation succeeded */
    REL_NO_CHANGE = 1,    /* Tuple already present (for add) */
    REL_NOT_FOUND = 2,    /* Tuple not found (for contains/remove) */
    REL_ERROR = -1,       /* General error */
    REL_OUT_OF_MEMORY = -2
} RelResult;

/*
 * Relation Provider interface.
 *
 * A provider backs a Datalog relation with a specialized data structure.
 * The engine calls these functions instead of using explicit tuple storage.
 *
 * Required functions: add, contains
 * Optional functions: lookup, iter_all, iter_delta, ack_delta, has_delta, destroy
 *
 * For implicit representations (like eqrel), iter_all may be expensive
 * as it must materialize all represented tuples.
 */
typedef struct RelProvider {
    /* Provider kind (for debugging and optimization decisions) */
    RelProviderKind kind;

    /* Provider-specific context (e.g., UnionFind struct for eqrel) */
    void *ctx;

    /* Arity of the relation (set during registration) */
    int arity;

    /*
     * Add a tuple to the relation (REQUIRED).
     *
     * For explicit storage: insert into hash set
     * For eqrel: call uf_union(fields[0], fields[1])
     *
     * @param ctx     Provider context
     * @param fields  Tuple values
     * @param arity   Number of fields
     * @return        REL_OK if tuple was added (changed the relation)
     *                REL_NO_CHANGE if tuple was already present
     *                REL_ERROR on failure
     */
    RelResult (*add)(void *ctx, const Value *fields, int arity);

    /*
     * Check if a tuple is in the relation (REQUIRED).
     *
     * For explicit storage: hash lookup
     * For eqrel: return uf_find(a) == uf_find(b)
     *
     * @param ctx     Provider context
     * @param fields  Tuple values (all must be bound/ground)
     * @param arity   Number of fields
     * @return        1 if tuple is in relation, 0 if not
     */
    int (*contains)(void *ctx, const Value *fields, int arity);

    /*
     * Lookup tuples matching a bound key (OPTIONAL).
     *
     * For eqrel with key_pos=0: enumerate all b where eq(key, b)
     * This is the equivalence class of key.
     *
     * @param ctx       Provider context
     * @param key       The bound key value
     * @param key_pos   Which argument position is bound (0 or 1 for binary)
     * @param emit      Callback to invoke for each matching tuple
     * @param user      Opaque pointer passed to emit
     */
    void (*lookup)(void *ctx, const Value *key, int key_pos,
                   RelEmitFn emit, void *user);

    /*
     * Iterate all tuples in the relation (OPTIONAL but recommended).
     *
     * WARNING: For implicit representations, this may be expensive!
     * For eqrel with N elements, this enumerates O(N^2) pairs.
     * Use only for output/debugging, not during evaluation.
     *
     * @param ctx   Provider context
     * @param emit  Callback for each tuple
     * @param user  Opaque pointer passed to emit
     */
    void (*iter_all)(void *ctx, RelEmitFn emit, void *user);

    /*
     * Check if there are changes since last ack_delta (OPTIONAL).
     * Used for semi-naive evaluation.
     *
     * @param ctx  Provider context
     * @return     1 if changes pending, 0 otherwise
     */
    int (*has_delta)(void *ctx);

    /*
     * Iterate tuples added since last ack_delta (OPTIONAL).
     *
     * For eqrel: iterate pairs that became equivalent due to recent unions.
     * This is the gamma_delta function from the BYODS formalism.
     *
     * @param ctx   Provider context
     * @param emit  Callback for each new tuple
     * @param user  Opaque pointer passed to emit
     */
    void (*iter_delta)(void *ctx, RelEmitFn emit, void *user);

    /*
     * Acknowledge that delta has been processed (OPTIONAL).
     * Called after each evaluation iteration.
     *
     * @param ctx  Provider context
     */
    void (*ack_delta)(void *ctx);

    /*
     * Destroy the provider and free resources (OPTIONAL).
     * Called when engine is destroyed.
     *
     * @param ctx  Provider context
     */
    void (*destroy)(void *ctx);

} RelProvider;

/*
 * Create an explicit (hash-based) provider for a relation.
 * This is the default - used when no specialized provider is configured.
 *
 * @param arity  Number of arguments in the relation
 * @return       New provider, or NULL on allocation failure
 */
RelProvider *rel_provider_create_explicit(int arity);

/*
 * Create an eqrel (union-find) provider for binary equivalence relations.
 *
 * @param initial_capacity  Initial size for union-find arrays
 * @return                  New provider, or NULL on allocation failure
 */
RelProvider *rel_provider_create_eqrel(size_t initial_capacity);

/*
 * Destroy a provider and free all resources.
 *
 * @param provider  Provider to destroy (may be NULL)
 */
void rel_provider_destroy(RelProvider *provider);

#ifdef __cplusplus
}
#endif

#endif /* RULR_REL_PROVIDER_H */
