#ifndef MINI_DATALOG_ENGINE_H
#define MINI_DATALOG_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common/types.h"
#include "frontend/ast.h"

typedef struct Engine Engine;

typedef int (*InternSymbolFn)(void *user, const char *s);
typedef const char *(*LookupSymbolFn)(void *user, int sym_id);

typedef struct {
    int  is_error;
    char message[256];
} EngineError;

Engine *engine_create(void);
void    engine_destroy(Engine *e);

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
