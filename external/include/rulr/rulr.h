#ifndef RULR_H
#define RULR_H

#include "engine/engine.h"
#include "runtime/runtime.h"
#include "frontend/ast.h"

typedef struct {
    char **names;
    int    count;
    int    capacity;
} RulrSymTable;

typedef struct {
    Engine      *engine;
    RulrSymTable symtab;
} Rulr;

typedef struct {
    int  is_error;
    char message[256];
} RulrError;

RulrError rulr_ok(void);
RulrError rulr_error(const char *message);

RulrError rulr_init(Rulr *r);
void      rulr_deinit(Rulr *r);

int         rulr_intern_symbol(Rulr *r, const char *s);
const char *rulr_lookup_symbol(const Rulr *r, int sym_id);

RulrError rulr_load_program(Rulr *r, const char *source);
RulrError rulr_evaluate(Rulr *r);

/**
 * Load rules from a pre-parsed AST (used for compiled rule files).
 */
RulrError rulr_load_program_ast(Rulr *r, const AstProgram *ast);

/**
 * Clear derived facts and rules, keeping only the base (injected) facts.
 * This allows loading a new rule file and re-evaluating with the same facts.
 */
void rulr_clear_derived(Rulr *r);

EngineRelationView rulr_get_relation(Rulr *r, const char *pred_name);

#endif /* RULR_H */
