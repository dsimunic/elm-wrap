#ifndef MINI_DATALOG_ENGINE_H
#define MINI_DATALOG_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common/types.h"
#include "frontend/ast.h"

typedef struct Engine Engine;

/**
 * External callback interface for the engine.
 * Allows host code to inject logic at the end of each iteration.
 */
typedef struct {
    /**
     * Called at the end of each semi-naive evaluation iteration.
     * @param engine The engine instance
     * @param stratum Current stratum being evaluated
     * @param user_data User-provided context
     * @param out_changed Set to non-zero if external changes occurred
     */
    void (*on_iteration_end)(Engine *engine, int stratum, void *user_data, int *out_changed);
    void *user_data;
} EngineExternal;

typedef int (*InternSymbolFn)(void *user, const char *s);
typedef const char *(*LookupSymbolFn)(void *user, int sym_id);

typedef struct {
    int  is_error;
    char message[256];
} EngineError;

Engine *engine_create(void);
void    engine_destroy(Engine *e);

/**
 * Register an external callback interface.
 * The external's callbacks will be invoked during evaluation.
 */
void engine_register_external(Engine *e, EngineExternal *external);

void engine_set_symbol_table(
    Engine          *e,
    InternSymbolFn   intern,
    LookupSymbolFn   lookup,
    void            *user_data
);

int engine_register_predicate(
    Engine             *e,
    const char         *name,
    int                 arity,
    const EngineArgType *types
);

int engine_get_predicate_id(Engine *e, const char *name);

int engine_insert_fact(Engine *e, int pred_id, int arity, const Value *values);

EngineError engine_load_rules_from_string(Engine *e, const char *source);
EngineError engine_load_rules_from_file(Engine *e, const char *path);

/**
 * Load rules from a pre-parsed AST (used for compiled rule files).
 */
EngineError engine_load_rules_from_ast(Engine *e, const AstProgram *ast);

/**
 * Clear all derived (IDB) facts from the engine while keeping base (EDB) facts.
 * This allows reloading new rules and re-evaluating with the same injected facts.
 */
void engine_clear_derived_facts(Engine *e);

EngineError engine_evaluate(Engine *e);

typedef struct {
    int         pred_id;
    int         num_tuples;
    const void *tuples;
} EngineRelationView;

EngineRelationView engine_get_relation_view(Engine *e, int pred_id);

#ifdef __cplusplus
}
#endif

#endif /* MINI_DATALOG_ENGINE_H */
