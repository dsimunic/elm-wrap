#ifndef RULR_REL_PROVIDER_EGRAPH_H
#define RULR_REL_PROVIDER_EGRAPH_H

/*
 * EGraph Provider - Congruence Closure backed relation
 *
 * Provides automatic congruence propagation: if A=B, then f(A)=f(B).
 * Wraps the union-find and congruence closure code from union-find/.
 *
 * Two types of providers share one EGraphCtx:
 * - term_eq: binary equivalence relation with congruence propagation
 * - enode1/2/3: function application facts (unary/binary/ternary)
 *
 * Usage:
 *   EGraphCtx *eg = egraph_ctx_create(1024);
 *   RelProvider *eq = rel_provider_create_egraph_eq(eg);
 *   RelProvider *enode2 = rel_provider_create_egraph_enode2(eg);
 *   engine_set_relation_provider(engine, "term_eq", eq);
 *   engine_set_relation_provider(engine, "enode2", enode2);
 */

#include "rel_provider.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque context shared by all egraph-backed providers */
typedef struct EGraphCtx EGraphCtx;

/*
 * Create a shared egraph context.
 *
 * @param initial_capacity  Initial size for internal arrays
 * @return                  New context, or NULL on allocation failure
 */
EGraphCtx *egraph_ctx_create(size_t initial_capacity);

/*
 * Destroy an egraph context and all associated providers.
 * Note: With arena allocation, this may be a no-op.
 *
 * @param ctx  Context to destroy (may be NULL)
 */
void egraph_ctx_destroy(EGraphCtx *ctx);

/*
 * Create provider for binary term equivalence relation.
 * Relation: term_eq(A, B) - terms A and B are equivalent.
 *
 * When eq(A, B) is added:
 * 1. A and B are unified in the union-find
 * 2. Congruence closure runs to discover f(A)=f(B) for all f
 * 3. New equivalences are tracked for semi-naive evaluation
 *
 * @param shared  Shared egraph context
 * @return        New provider, or NULL on failure
 */
RelProvider *rel_provider_create_egraph_eq(EGraphCtx *shared);

/*
 * Create provider for unary function applications.
 * Relation: enode1(opcode, arg, result) - opcode(arg) = result
 *
 * @param shared  Shared egraph context
 * @return        New provider, or NULL on failure
 */
RelProvider *rel_provider_create_egraph_enode1(EGraphCtx *shared);

/*
 * Create provider for binary function applications.
 * Relation: enode2(opcode, arg0, arg1, result) - opcode(arg0, arg1) = result
 *
 * @param shared  Shared egraph context
 * @return        New provider, or NULL on failure
 */
RelProvider *rel_provider_create_egraph_enode2(EGraphCtx *shared);

/*
 * Create provider for ternary function applications.
 * Relation: enode3(opcode, arg0, arg1, arg2, result) - opcode(arg0, arg1, arg2) = result
 *
 * @param shared  Shared egraph context
 * @return        New provider, or NULL on failure
 */
RelProvider *rel_provider_create_egraph_enode3(EGraphCtx *shared);

/*
 * Get statistics about the egraph.
 *
 * @param ctx             Egraph context
 * @param out_num_eclasses  Number of equivalence classes (optional, may be NULL)
 * @param out_num_enodes    Number of registered e-nodes (optional, may be NULL)
 * @param out_num_pending   Number of pending delta equivalences (optional, may be NULL)
 */
void egraph_ctx_stats(EGraphCtx *ctx, size_t *out_num_eclasses,
                      size_t *out_num_enodes, size_t *out_num_pending);

/*
 * Standard opcodes for expression e-nodes.
 * Applications can define additional opcodes starting from OP_USER.
 */
enum EGraphOpcode {
    OP_NONE = 0,
    OP_ADD = 1,
    OP_SUB = 2,
    OP_MUL = 3,
    OP_DIV = 4,
    OP_NEG = 5,      /* Unary negation */
    OP_APP = 6,      /* Function application */
    OP_IF = 7,       /* Ternary if-then-else */
    OP_CONS = 8,     /* List cons */
    OP_TUPLE = 9,    /* Tuple construction */
    OP_USER = 100    /* User-defined opcodes start here */
};

#ifdef __cplusplus
}
#endif

#endif /* RULR_REL_PROVIDER_EGRAPH_H */
