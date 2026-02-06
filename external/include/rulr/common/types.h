#ifndef MINI_DATALOG_TYPES_H
#define MINI_DATALOG_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_ARITY 8
#define MAX_PREDICATES 128
#define MAX_LITERALS 32
#define MAX_RULES 256
#define MAX_VARS 32

typedef enum {
    VAL_SYM,
    VAL_INT,
    VAL_RANGE,
    VAL_FACT   /* Nested fact (first-class fact) - stores InternId */
} ValueKind;

typedef struct {
    ValueKind kind;
    union {
        int      sym;       /* Symbol ID (VAL_SYM) */
        long     i;         /* Integer or Range ID (VAL_INT, VAL_RANGE) */
        uint64_t fact_id;   /* InternId for nested fact (VAL_FACT) */
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

static inline Value make_fact_value(uint64_t fact_id) {
    Value v;
    v.kind = VAL_FACT;
    v.u.fact_id = fact_id;
    return v;
}

static inline bool value_equal(Value a, Value b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case VAL_SYM:
        return a.u.sym == b.u.sym;
    case VAL_FACT:
        return a.u.fact_id == b.u.fact_id;
    default:
        return a.u.i == b.u.i;
    }
}

#endif /* MINI_DATALOG_TYPES_H */
