#ifndef MINI_DATALOG_AST_H
#define MINI_DATALOG_AST_H

#include "common/types.h"

typedef enum {
    AST_DECL,
    AST_FACT,
    AST_RULE
} AstNodeKind;

typedef struct {
    char *name;
    int   arity;
    char **arg_names;
    char **arg_types;
} AstDecl;

typedef enum {
    AST_ARG_STRING,
    AST_ARG_INT
} AstArgKind;

typedef struct {
    char *pred;
    int   arity;
    AstArgKind arg_kind[MAX_ARITY];
    union {
        char *s;
        long  i;
    } arg_value[MAX_ARITY];
} AstFact;

typedef struct {
    char *name;
    int   id;
} AstVar;

typedef enum {
    TERM_VAR,
    TERM_STRING,
    TERM_INT,
    TERM_WILDCARD
} AstTermKind;

typedef struct {
    AstTermKind kind;
    union {
        AstVar var;
        char  *s;
        long   i;
    } u;
} AstTerm;

typedef enum {
    LIT_POS,
    LIT_NEG,
    LIT_EQ,
    LIT_CMP,      /* Comparison: <, <=, >, >=, != */
    LIT_BUILTIN   /* Builtin call: match(pattern, string) */
} AstLitKind;

typedef enum {
    CMP_EQ,       /* = (same as LIT_EQ but for uniformity) */
    CMP_NE,       /* != or <> */
    CMP_LT,       /* < */
    CMP_LE,       /* <= */
    CMP_GT,       /* > */
    CMP_GE        /* >= */
} AstCmpOp;

typedef enum {
    BUILTIN_MATCH /* match(pattern, string) - regex match */
} AstBuiltinKind;

typedef struct {
    AstLitKind kind;
    char      *pred;
    int        arity;
    AstTerm    args[MAX_ARITY];
    AstTerm    lhs;
    AstTerm    rhs;
    AstCmpOp   cmp_op;         /* For LIT_CMP */
    AstBuiltinKind builtin;    /* For LIT_BUILTIN */
} AstLiteral;

typedef struct {
    char      *head_pred;
    int        head_arity;
    AstTerm    head_args[MAX_ARITY];
    int        num_body;
    int        body_capacity;
    AstLiteral *body;
} AstRule;

typedef struct {
    int       is_error;
    char      message[256];
} ParseError;

typedef struct {
    AstDecl  *decls;
    int       num_decls;
    int       decls_capacity;

    AstFact  *facts;
    int       num_facts;
    int       facts_capacity;

    AstRule  *rules;
    int       num_rules;
    int       rules_capacity;

    int       clear_derived;  /* 1 if .clear_derived() directive was found */
} AstProgram;

void ast_program_init(AstProgram *prog);
void ast_program_reset(AstProgram *prog);
ParseError parse_program(const char *source, AstProgram *prog);

#endif /* MINI_DATALOG_AST_H */
