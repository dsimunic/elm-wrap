/**
 * host_helpers.c - Rulr host fact insertion helpers
 */

#include "host_helpers.h"
#include "engine/engine.h"
#include "common/types.h"

int rulr_insert_fact_1s(Rulr *r, const char *pred, const char *s1) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        EngineArgType types[] = {ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 1, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    if (sym1 < 0) return -1;

    Value vals[1];
    vals[0] = make_sym_value(sym1);
    return engine_insert_fact(r->engine, pid, 1, vals);
}

int rulr_insert_fact_2s(Rulr *r, const char *pred, const char *s1, const char *s2) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        EngineArgType types[] = {ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 2, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    int sym2 = rulr_intern_symbol(r, s2);
    if (sym1 < 0 || sym2 < 0) return -1;

    Value vals[2];
    vals[0] = make_sym_value(sym1);
    vals[1] = make_sym_value(sym2);
    return engine_insert_fact(r->engine, pid, 2, vals);
}

int rulr_insert_fact_3s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        EngineArgType types[] = {ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 3, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    int sym2 = rulr_intern_symbol(r, s2);
    int sym3 = rulr_intern_symbol(r, s3);
    if (sym1 < 0 || sym2 < 0 || sym3 < 0) return -1;

    Value vals[3];
    vals[0] = make_sym_value(sym1);
    vals[1] = make_sym_value(sym2);
    vals[2] = make_sym_value(sym3);
    return engine_insert_fact(r->engine, pid, 3, vals);
}

int rulr_insert_fact_4s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3, const char *s4) {
    int pid = engine_get_predicate_id(r->engine, pred);
    if (pid < 0) {
        EngineArgType types[] = {ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL, ARG_TYPE_SYMBOL};
        pid = engine_register_predicate(r->engine, pred, 4, types);
    }
    if (pid < 0) return -1;

    int sym1 = rulr_intern_symbol(r, s1);
    int sym2 = rulr_intern_symbol(r, s2);
    int sym3 = rulr_intern_symbol(r, s3);
    int sym4 = rulr_intern_symbol(r, s4);
    if (sym1 < 0 || sym2 < 0 || sym3 < 0 || sym4 < 0) return -1;

    Value vals[4];
    vals[0] = make_sym_value(sym1);
    vals[1] = make_sym_value(sym2);
    vals[2] = make_sym_value(sym3);
    vals[3] = make_sym_value(sym4);
    return engine_insert_fact(r->engine, pid, 4, vals);
}
