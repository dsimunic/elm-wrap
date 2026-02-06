#ifndef RULR_BYODS_H
#define RULR_BYODS_H

/*
 * BYODS (Bring Your Own Data Structures) - External Data Provider Interface
 *
 * This module allows host applications to expose their native data structures
 * as Datalog EDB (extensional database) relations without converting them to
 * Rulr's internal tuple format.
 *
 * The host implements the ByodsProvider interface, and Rulr calls through it
 * during query evaluation. This is similar to how LuaJIT FFI works: provide
 * a declarative spec, and the system generates the necessary interface calls.
 *
 * Minimal prototype: only iter_all is required. Indexed lookups will be added
 * in a future iteration.
 */

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Callback function type for emitting tuples during iteration.
 * The provider calls this once for each tuple it wants to yield.
 *
 * @param fields  Array of Values representing the tuple
 * @param arity   Number of fields in the tuple
 * @param user    User data passed through from the iteration call
 *
 * @return 0 to continue iteration, non-zero to stop early
 */
typedef int (*ByodsEmitFn)(const Value *fields, int arity, void *user);

/*
 * Provider interface for an external predicate.
 *
 * The host allocates this struct and fills in the function pointers,
 * then registers it with the engine via engine_register_byods_provider().
 */
typedef struct {
    /*
     * User-provided context pointer.
     * This is passed as the first argument to all callback functions.
     * Typically points to the host's data structure.
     */
    void *ctx;

    /*
     * Iterate all tuples in the relation (required).
     * This is the concretization function γ from the BYODS formalism.
     *
     * @param ctx   The context pointer from this struct
     * @param emit  Callback to invoke for each tuple
     * @param user  Opaque pointer to pass through to emit()
     */
    void (*iter_all)(void *ctx, ByodsEmitFn emit, void *user);

    /*
     * Iterate tuples added since last acknowledgment (optional).
     * Used for incremental re-evaluation. NULL if not supported.
     */
    void (*iter_delta)(void *ctx, ByodsEmitFn emit, void *user);

    /*
     * Acknowledge that the current delta has been processed (optional).
     * Called after evaluation completes. NULL if not supported.
     */
    void (*ack_delta)(void *ctx);

    /*
     * Check if there are pending changes since last acknowledgment (optional).
     * Used to determine whether incremental re-evaluation is possible.
     *
     * @param ctx  The context pointer
     * @return     1 if there are pending changes, 0 otherwise
     */
    int (*has_delta)(void *ctx);

    /*
     * Lookup by first argument (optional, for indexed access).
     * If provided, enables efficient joins when arg0 is bound.
     *
     * @param ctx       The context pointer
     * @param key       The key value to look up
     * @param emit      Callback to invoke for each matching tuple
     * @param user      Opaque pointer to pass through to emit()
     */
    void (*lookup_arg0)(void *ctx, Value key, ByodsEmitFn emit, void *user);

    /*
     * Lookup by second argument (optional, for indexed access).
     * If provided, enables efficient joins when arg1 is bound.
     */
    void (*lookup_arg1)(void *ctx, Value key, ByodsEmitFn emit, void *user);

} ByodsProvider;

/*
 * Maximum number of external predicates that can be registered.
 */
#define MAX_BYODS_PROVIDERS 32

/*
 * Registry of all registered BYODS providers.
 * Stored in the Engine struct.
 */
typedef struct {
    ByodsProvider providers[MAX_BYODS_PROVIDERS];
    int           pred_ids[MAX_BYODS_PROVIDERS];  /* Maps provider index to pred_id */
    int           count;
} ByodsRegistry;

/*
 * Initialize an empty BYODS registry.
 */
static inline void byods_registry_init(ByodsRegistry *reg) {
    reg->count = 0;
    for (int i = 0; i < MAX_BYODS_PROVIDERS; i++) {
        reg->pred_ids[i] = -1;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* RULR_BYODS_H */
