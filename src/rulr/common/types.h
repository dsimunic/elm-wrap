#ifndef MINI_DATALOG_TYPES_H
#define MINI_DATALOG_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#define MAX_ARITY 8
#define MAX_PREDICATES 128
#define MAX_LITERALS 32
#define MAX_RULES 256
#define MAX_VARS 32

typedef enum {
    VAL_SYM,
    VAL_INT,
    VAL_RANGE
} ValueKind;

typedef struct {
    ValueKind kind;
    union {
        int  sym;
        long i;
    } u;
} Value;

typedef enum {
    ARG_TYPE_SYMBOL,
    ARG_TYPE_INT,
    ARG_TYPE_RANGE,
    ARG_TYPE_UNKNOWN
} EngineArgType;

static inline Value make_sym_value(int sym_id) {
    Value v;
    v.kind = VAL_SYM;
    v.u.sym = sym_id;
    return v;
}

static inline Value make_int_value(long i) {
    Value v;
    v.kind = VAL_INT;
    v.u.i = i;
    return v;
}

static inline Value make_range_value(long i) {
    Value v;
    v.kind = VAL_RANGE;
    v.u.i = i;
    return v;
}

static inline bool value_equal(Value a, Value b) {
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind == VAL_SYM) {
        return a.u.sym == b.u.sym;
    }
    return a.u.i == b.u.i;
}

#endif /* MINI_DATALOG_TYPES_H */
