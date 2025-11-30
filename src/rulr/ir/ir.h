#ifndef MINI_DATALOG_IR_H
#define MINI_DATALOG_IR_H

#include "common/types.h"

typedef int PredId;

typedef struct {
    char          *name;
    int            arity;
    EngineArgType  arg_types[MAX_ARITY];
    int            declared;
    int            stratum;
    int            is_idb;      /* 1 if this predicate appears as a rule head */
} PredDef;

typedef struct {
    PredDef preds[MAX_PREDICATES];
    int     count;
} PredTable;

typedef enum {
    IR_TERM_VAR,
    IR_TERM_SYM,
    IR_TERM_INT
} IrTermKind;

typedef struct {
    IrTermKind kind;
    int        var; /* for VAR */
    int        sym; /* for SYM */
    long       i;   /* for INT */
} IrTerm;

typedef enum {
    IR_LIT_POS,
    IR_LIT_NEG,
    IR_LIT_EQ
} IrLitKind;

typedef struct {
    IrLitKind kind;
    PredId    pred;
    int       arity;
    IrTerm    args[MAX_ARITY];
    IrTerm    lhs;
    IrTerm    rhs;
} IrLiteral;

typedef struct {
    PredId    head_pred;
    int       head_arity;
    IrTerm    head_args[MAX_ARITY];
    int       num_body;
    IrLiteral body[MAX_LITERALS];
    int       num_vars;
} IrRule;

typedef struct {
    PredTable pred_table;
    int       num_rules;
    int       max_stratum;
    IrRule    rules[MAX_RULES];
} IrProgram;

#endif /* MINI_DATALOG_IR_H */
