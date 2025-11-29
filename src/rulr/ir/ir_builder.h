#ifndef MINI_DATALOG_IR_BUILDER_H
#define MINI_DATALOG_IR_BUILDER_H

#include "frontend/ast.h"
#include "ir/ir.h"
#include "engine/engine.h"

void ir_program_init(IrProgram *prog);
EngineError ir_build_from_ast(const AstProgram *ast, IrProgram *prog, InternSymbolFn intern, void *user_data);
EngineError ir_register_predicate(
    PredTable *pt,
    const char *name,
    int arity,
    const EngineArgType *types,
    int declared,
    PredId *out_id
);
int ir_find_predicate(const PredTable *pt, const char *name);

#endif /* MINI_DATALOG_IR_BUILDER_H */
