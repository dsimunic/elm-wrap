#include "rulr.h"

#include <string.h>

#include "alloc.h"
#include "common/dyn_array.h"

static int sym_intern(void *user, const char *s) {
    RulrSymTable *tab = (RulrSymTable *)user;
    for (int i = 0; i < tab->count; ++i) {
        if (strcmp(tab->names[i], s) == 0) {
            return i;
        }
    }
    char *copy = arena_strdup(s);
    if (!copy) {
        return -1;
    }
    if (DYNARRAY_PUSH(tab->names, tab->count, tab->capacity, copy) < 0) {
        return -1;
    }
    return tab->count - 1;
}

static const char *sym_lookup(void *user, int sym_id) {
    RulrSymTable *tab = (RulrSymTable *)user;
    if (sym_id < 0 || sym_id >= tab->count) {
        return NULL;
    }
    return tab->names[sym_id];
}

RulrError rulr_ok(void) {
    RulrError err;
    err.is_error = 0;
    err.message[0] = '\0';
    return err;
}

RulrError rulr_error(const char *message) {
    RulrError err;
    err.is_error = 1;
    strncpy(err.message, message ? message : "unknown error", sizeof(err.message) - 1);
    err.message[sizeof(err.message) - 1] = '\0';
    return err;
}

static RulrError from_engine_error(EngineError err) {
    if (!err.is_error) {
        return rulr_ok();
    }
    return rulr_error(err.message);
}

RulrError rulr_init(Rulr *r) {
    if (!r) {
        return rulr_error("Invalid Rulr pointer");
    }
    r->engine = engine_create();
    if (!r->engine) {
        return rulr_error("Failed to create engine");
    }
    r->symtab.names = NULL;
    r->symtab.count = 0;
    r->symtab.capacity = 0;
    engine_set_symbol_table(r->engine, sym_intern, sym_lookup, &r->symtab);
    return rulr_ok();
}

void rulr_deinit(Rulr *r) {
    if (!r) {
        return;
    }
    engine_destroy(r->engine);
    r->engine = NULL;
    r->symtab.names = NULL;
    r->symtab.count = 0;
    r->symtab.capacity = 0;
}

int rulr_intern_symbol(Rulr *r, const char *s) {
    if (!r) {
        return -1;
    }
    return sym_intern(&r->symtab, s);
}

const char *rulr_lookup_symbol(const Rulr *r, int sym_id) {
    if (!r) {
        return NULL;
    }
    return sym_lookup((void *)&r->symtab, sym_id);
}

RulrError rulr_load_program(Rulr *r, const char *source) {
    if (!r || !source) {
        return rulr_error("Invalid input to rulr_load_program");
    }
    EngineError err = engine_load_rules_from_string(r->engine, source);
    return from_engine_error(err);
}

RulrError rulr_load_program_ast(Rulr *r, const AstProgram *ast) {
    if (!r || !ast) {
        return rulr_error("Invalid input to rulr_load_program_ast");
    }
    EngineError err = engine_load_rules_from_ast(r->engine, ast);
    return from_engine_error(err);
}

RulrError rulr_evaluate(Rulr *r) {
    if (!r) {
        return rulr_error("Invalid Rulr pointer");
    }
    EngineError err = engine_evaluate(r->engine);
    return from_engine_error(err);
}

void rulr_clear_derived(Rulr *r) {
    if (!r) return;
    engine_clear_derived_facts(r->engine);
}

EngineRelationView rulr_get_relation(Rulr *r, const char *pred_name) {
    EngineRelationView view;
    view.pred_id = -1;
    view.num_tuples = 0;
    view.tuples = NULL;
    if (!r || !pred_name) {
        return view;
    }
    int pid = engine_get_predicate_id(r->engine, pred_name);
    if (pid < 0) {
        return view;
    }
    return engine_get_relation_view(r->engine, pid);
}
