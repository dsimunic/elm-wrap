#include <stdio.h>
#include <string.h>
#include "engine/engine.h"
#include "frontend/ast.h"
#include "ir/ir_builder.h"
#include "runtime/runtime.h"
#include "common/dyn_array.h"
#include "alloc.h"

struct Engine {
    InternSymbolFn intern;
    LookupSymbolFn lookup;
    void *sym_user;

    IrProgram   prog;
    PredRuntime preds[MAX_PREDICATES];
    int         num_preds;

    char **sym_names;
    int    sym_count;
    int    sym_capacity;
};

static EngineError engine_ok(void) {
    EngineError err;
    err.is_error = 0;
    err.message[0] = '\0';
    return err;
}

static EngineError engine_err(const char *msg) {
    EngineError err;
    err.is_error = 1;
    snprintf(err.message, sizeof(err.message), "%s", msg);
    return err;
}

static int default_intern(void *user, const char *s) {
    Engine *e = (Engine *)user;
    for (int i = 0; i < e->sym_count; ++i) {
        if (strcmp(e->sym_names[i], s) == 0) {
            return i;
        }
    }
    if (DYNARRAY_PUSH(e->sym_names, e->sym_count, e->sym_capacity, arena_strdup(s)) < 0) {
        return -1;
    }
    return e->sym_count - 1;
}

static const char *default_lookup(void *user, int sym_id) {
    Engine *e = (Engine *)user;
    if (sym_id < 0 || sym_id >= e->sym_count) {
        return NULL;
    }
    return e->sym_names[sym_id];
}

Engine *engine_create(void) {
    Engine *e = (Engine *)arena_calloc(1, sizeof(Engine));
    if (!e) {
        return NULL;
    }
    ir_program_init(&e->prog);
    e->intern = default_intern;
    e->lookup = default_lookup;
    e->sym_user = e;
    e->num_preds = 0;
    e->sym_names = NULL;
    e->sym_count = 0;
    e->sym_capacity = 0;
    return e;
}

void engine_destroy(Engine *e) {
    (void)e;
}

void engine_set_symbol_table(
    Engine          *e,
    InternSymbolFn   intern,
    LookupSymbolFn   lookup,
    void            *user_data
) {
    if (!e) {
        return;
    }
    e->intern = intern ? intern : default_intern;
    e->lookup = lookup ? lookup : default_lookup;
    e->sym_user = user_data ? user_data : e;
}

static PredRuntime *engine_prepare_pred_runtime(Engine *e, PredId pid) {
    if (pid < 0 || pid >= MAX_PREDICATES) {
        return NULL;
    }
    while (e->num_preds <= pid) {
        e->num_preds += 1;
    }
    PredDef *pd = &e->prog.pred_table.preds[pid];
    PredRuntime *pr = &e->preds[pid];
    if (pr->rel.base.items == NULL && pd->arity >= 0) {
        relation_init(pr, pd->arity);
    } else if (pd->arity >= 0 && pr->arity != pd->arity) {
        return NULL;
    }
    pr->stratum = pd->stratum;
    return pr;
}

int engine_register_predicate(
    Engine             *e,
    const char         *name,
    int                 arity,
    const EngineArgType *types
) {
    if (!e || !name) {
        return -1;
    }
    EngineArgType tmp[MAX_ARITY];
    for (int i = 0; i < MAX_ARITY; ++i) {
        tmp[i] = ARG_TYPE_UNKNOWN;
    }
    for (int i = 0; i < arity && i < MAX_ARITY; ++i) {
        if (types) {
            tmp[i] = types[i];
        }
    }
    PredId pid;
    EngineError err = ir_register_predicate(&e->prog.pred_table, name, arity, tmp, 1, &pid);
    if (err.is_error) {
        return -1;
    }
    if (engine_prepare_pred_runtime(e, pid) == NULL) {
        return -1;
    }
    return pid;
}

int engine_get_predicate_id(Engine *e, const char *name) {
    if (!e) {
        return -1;
    }
    return ir_find_predicate(&e->prog.pred_table, name);
}

static int value_matches_argtype(Value v, EngineArgType type) {
    if (type == ARG_TYPE_UNKNOWN) {
        return 1;
    }
    if (type == ARG_TYPE_SYMBOL) {
        return v.kind == VAL_SYM;
    }
    if (type == ARG_TYPE_INT) {
        return v.kind == VAL_INT;
    }
    if (type == ARG_TYPE_RANGE) {
        return v.kind == VAL_RANGE || v.kind == VAL_INT;
    }
    return 0;
}

int engine_insert_fact(Engine *e, int pred_id, int arity, const Value *values) {
    if (!e || pred_id < 0 || pred_id >= e->prog.pred_table.count) {
        return -1;
    }
    PredDef *pd = &e->prog.pred_table.preds[pred_id];
    if (pd->arity != arity) {
        return -1;
    }
    PredRuntime *pr = engine_prepare_pred_runtime(e, pred_id);
    if (!pr) {
        return -1;
    }

    Tuple t;
    t.arity = arity;
    for (int i = 0; i < arity; ++i) {
        if (!value_matches_argtype(values[i], pd->arg_types[i])) {
            return -1;
        }
        t.fields[i] = values[i];
    }
    return relation_base_insert_unique(pr, &t);
}

static EngineError load_file_contents(const char *path, char **out_buffer) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return engine_err("Failed to open rule file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return engine_err("Failed to seek rule file");
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return engine_err("Failed to stat rule file");
    }
    rewind(f);
    char *buf = (char *)arena_malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return engine_err("Out of memory");
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    *out_buffer = buf;
    return engine_ok();
}

static EngineError sync_runtime(Engine *e) {
    for (int i = 0; i < e->prog.pred_table.count; ++i) {
        PredRuntime *pr = engine_prepare_pred_runtime(e, i);
        if (!pr) {
            return engine_err("Failed to prepare predicate runtime");
        }
        pr->stratum = e->prog.pred_table.preds[i].stratum;
    }
    return engine_ok();
}

static EngineError insert_ast_facts(Engine *e, const AstProgram *ast) {
    for (int i = 0; i < ast->num_facts; ++i) {
        const AstFact *fact = &ast->facts[i];
        int pid = ir_find_predicate(&e->prog.pred_table, fact->pred);
        if (pid < 0) {
            return engine_err("Unknown predicate in fact");
        }
        PredDef *pd = &e->prog.pred_table.preds[pid];
        if (pd->arity != fact->arity) {
            return engine_err("Fact arity mismatch");
        }
        Value values[MAX_ARITY];
        for (int a = 0; a < fact->arity; ++a) {
            if (fact->arg_kind[a] == AST_ARG_STRING) {
                int sym = e->intern(e->sym_user, fact->arg_value[a].s);
                values[a] = make_sym_value(sym);
            } else {
                values[a] = make_int_value(fact->arg_value[a].i);
            }
        }
        int res = engine_insert_fact(e, pid, fact->arity, values);
        if (res < 0) {
            return engine_err("Failed to insert fact");
        }
    }
    return engine_ok();
}

EngineError engine_load_rules_from_string(Engine *e, const char *source) {
    if (!e || !source) {
        return engine_err("Invalid engine or source");
    }

    AstProgram ast;
    ast_program_init(&ast);
    ParseError perr = parse_program(source, &ast);
    if (perr.is_error) {
        EngineError err = engine_err(perr.message);
        return err;
    }

    IrProgram new_prog = e->prog;
    new_prog.num_rules = 0;
    new_prog.max_stratum = 0;

    EngineError err = ir_build_from_ast(&ast, &new_prog, e->intern, e->sym_user);
    if (err.is_error) {
        return err;
    }

    e->prog = new_prog;
    EngineError sync_err = sync_runtime(e);
    if (sync_err.is_error) {
        return sync_err;
    }

    return insert_ast_facts(e, &ast);
}

EngineError engine_load_rules_from_file(Engine *e, const char *path) {
    char *buffer = NULL;
    EngineError err = load_file_contents(path, &buffer);
    if (err.is_error) {
        return err;
    }
    return engine_load_rules_from_string(e, buffer);
}

typedef struct {
    int   num_vars;
    bool  bound[MAX_VARS];
    Value values[MAX_VARS];
} Env;

static void env_init(Env *env, int num_vars) {
    env->num_vars = num_vars;
    for (int i = 0; i < num_vars; ++i) {
        env->bound[i] = false;
    }
}

static int eval_term_value(const IrTerm *term, const Env *env, Value *out) {
    if (term->kind == IR_TERM_INT) {
        *out = make_int_value(term->i);
        return 1;
    }
    if (term->kind == IR_TERM_SYM) {
        *out = make_sym_value(term->sym);
        return 1;
    }
    if (term->kind == IR_TERM_VAR) {
        if (term->var < 0 || term->var >= env->num_vars) {
            return 0;
        }
        if (!env->bound[term->var]) {
            return 0;
        }
        *out = env->values[term->var];
        return 1;
    }
    return 0;
}

static int bind_term_to_value(const IrTerm *term, const Value *val, Env *env) {
    if (term->kind == IR_TERM_VAR) {
        if (term->var < 0 || term->var >= env->num_vars) {
            return 0;
        }
        if (!env->bound[term->var]) {
            env->values[term->var] = *val;
            env->bound[term->var] = true;
            return 1;
        }
        return value_equal(env->values[term->var], *val);
    }
    if (term->kind == IR_TERM_INT) {
        return val->kind == VAL_INT && val->u.i == term->i;
    }
    if (term->kind == IR_TERM_SYM) {
        return val->kind == VAL_SYM && val->u.sym == term->sym;
    }
    return 0;
}

static int match_literal_with_tuple(const IrLiteral *lit, const Tuple *t, Env *env) {
    for (int i = 0; i < lit->arity; ++i) {
        if (!bind_term_to_value(&lit->args[i], &t->fields[i], env)) {
            return 0;
        }
    }
    return 1;
}

static int eval_eq_literal(const IrLiteral *lit, Env *env) {
    Value lhs, rhs;
    if (!eval_term_value(&lit->lhs, env, &lhs)) {
        return 0;
    }
    if (!eval_term_value(&lit->rhs, env, &rhs)) {
        return 0;
    }
    return value_equal(lhs, rhs);
}

static int exists_matching_tuple(PredRuntime *pr, const IrLiteral *lit, Env *env) {
    if (lit->arity == 0) {
        return pr->rel.base.size > 0;
    }
    Value key;
    int has_key = eval_term_value(&lit->args[0], env, &key);
    if (has_key && pr->index_enabled) {
        long k = key.kind == VAL_SYM ? key.u.sym : key.u.i;
        IntVector *rows = hash_index_lookup(&pr->idx_on_arg0, k);
        if (!rows) {
            return 0;
        }
        for (int i = 0; i < rows->count; ++i) {
            Tuple *t = &pr->rel.base.items[rows->indices[i]];
            Env env_copy = *env;
            if (match_literal_with_tuple(lit, t, &env_copy)) {
                return 1;
            }
        }
        return 0;
    }

    for (int i = 0; i < pr->rel.base.size; ++i) {
        Tuple *t = &pr->rel.base.items[i];
        Env env_copy = *env;
        if (match_literal_with_tuple(lit, t, &env_copy)) {
            return 1;
        }
    }
    return 0;
}

static int emit_head(Engine *e, const IrRule *rule, Env *env) {
    PredRuntime *pr = &e->preds[rule->head_pred];
    Tuple t;
    t.arity = rule->head_arity;
    for (int i = 0; i < rule->head_arity; ++i) {
        if (!eval_term_value(&rule->head_args[i], env, &t.fields[i])) {
            return 0;
        }
    }
    return relation_next_insert_unique(pr, &t);
}

static int eval_positive_literal(Engine *e, const IrLiteral *lit, const IrRule *rule, int lit_idx, int driver_idx, Env *env);

static int match_body_lit(Engine *e, const IrRule *rule, int lit_idx, int driver_idx, Env *env) {
    if (lit_idx >= rule->num_body) {
        return emit_head(e, rule, env);
    }
    if (lit_idx == driver_idx) {
        return match_body_lit(e, rule, lit_idx + 1, driver_idx, env);
    }

    const IrLiteral *lit = &rule->body[lit_idx];
    if (lit->kind == IR_LIT_POS) {
        return eval_positive_literal(e, lit, rule, lit_idx, driver_idx, env);
    }
    if (lit->kind == IR_LIT_NEG) {
        PredRuntime *pr = &e->preds[lit->pred];
        if (!exists_matching_tuple(pr, lit, env)) {
            return match_body_lit(e, rule, lit_idx + 1, driver_idx, env);
        }
        return 0;
    }
    if (lit->kind == IR_LIT_EQ) {
        if (eval_eq_literal(lit, env)) {
            return match_body_lit(e, rule, lit_idx + 1, driver_idx, env);
        }
        return 0;
    }
    return 0;
}

static int eval_positive_literal(Engine *e, const IrLiteral *lit, const IrRule *rule, int lit_idx, int driver_idx, Env *env) {
    PredRuntime *pr = &e->preds[lit->pred];
    int produced = 0;

    if (lit->arity > 0 && pr->index_enabled) {
        Value key;
        if (eval_term_value(&lit->args[0], env, &key)) {
            long k = key.kind == VAL_SYM ? key.u.sym : key.u.i;
            IntVector *rows = hash_index_lookup(&pr->idx_on_arg0, k);
            if (rows) {
                for (int i = 0; i < rows->count; ++i) {
                    Tuple *t = &pr->rel.base.items[rows->indices[i]];
                    Env env_copy = *env;
                    if (match_literal_with_tuple(lit, t, &env_copy)) {
                        produced |= match_body_lit(e, rule, lit_idx + 1, driver_idx, &env_copy);
                    }
                }
                return produced;
            }
        }
    }

    for (int i = 0; i < pr->rel.base.size; ++i) {
        Tuple *t = &pr->rel.base.items[i];
        Env env_copy = *env;
        if (match_literal_with_tuple(lit, t, &env_copy)) {
            produced |= match_body_lit(e, rule, lit_idx + 1, driver_idx, &env_copy);
        }
    }
    return produced;
}

static int find_driver_literal(const IrRule *rule) {
    for (int i = 0; i < rule->num_body; ++i) {
        if (rule->body[i].kind == IR_LIT_POS) {
            return i;
        }
    }
    return -1;
}

static int evaluate_rule(Engine *e, const IrRule *rule) {
    int driver_idx = find_driver_literal(rule);
    if (driver_idx < 0) {
        Env env;
        env_init(&env, rule->num_vars);
        return match_body_lit(e, rule, 0, -1, &env);
    }

    const IrLiteral *driver = &rule->body[driver_idx];
    PredRuntime *driver_pr = &e->preds[driver->pred];
    PredDef *driver_pd = &e->prog.pred_table.preds[driver->pred];
    int head_stratum = e->prog.pred_table.preds[rule->head_pred].stratum;
    TupleBuffer *driver_buf = &driver_pr->rel.delta;
    /* Use base instead of delta for:
     * - Lower stratum predicates (they're already fully computed)
     * - EDB predicates (they have no rules deriving them, so delta gets empty after first iteration) */
    if (driver_pr->stratum < head_stratum || !driver_pd->is_idb) {
        driver_buf = &driver_pr->rel.base;
    }

    int produced = 0;
    for (int i = 0; i < driver_buf->size; ++i) {
        Tuple *t = &driver_buf->items[i];
        Env env;
        env_init(&env, rule->num_vars);
        if (!match_literal_with_tuple(driver, t, &env)) {
            continue;
        }
        produced |= match_body_lit(e, rule, 0, driver_idx, &env);
    }
    return produced;
}

static void clear_next_for_stratum(Engine *e, int stratum) {
    for (int i = 0; i < e->num_preds; ++i) {
        if (e->preds[i].stratum == stratum) {
            tuple_buffer_clear(&e->preds[i].rel.next);
        }
    }
}

static EngineError init_deltas_for_stratum(Engine *e, int stratum) {
    for (int i = 0; i < e->num_preds; ++i) {
        if (e->preds[i].stratum == stratum) {
            if (relation_prepare_delta_from_base(&e->preds[i]) < 0) {
                return engine_err("Failed to init delta");
            }
        }
    }
    return engine_ok();
}

static int promote_next_for_stratum(Engine *e, int stratum) {
    int any_new = 0;
    for (int i = 0; i < e->num_preds; ++i) {
        if (e->preds[i].stratum == stratum) {
            int res = relation_promote_next(&e->preds[i]);
            if (res < 0) {
                return -1;
            }
            if (res > 0) {
                any_new = 1;
            }
        }
    }
    return any_new;
}

EngineError engine_evaluate(Engine *e) {
    if (!e) {
        return engine_err("Engine is NULL");
    }
    EngineError err = sync_runtime(e);
    if (err.is_error) {
        return err;
    }

    int max_stratum = e->prog.max_stratum;
    for (int s = 0; s <= max_stratum; ++s) {
        EngineError init_err = init_deltas_for_stratum(e, s);
        if (init_err.is_error) {
            return init_err;
        }
        int changed;
        do {
            clear_next_for_stratum(e, s);
            changed = 0;
            for (int r = 0; r < e->prog.num_rules; ++r) {
                IrRule *rule = &e->prog.rules[r];
                if (e->prog.pred_table.preds[rule->head_pred].stratum != s) {
                    continue;
                }
                int res = evaluate_rule(e, rule);
                if (res > 0) {
                    changed = 1;
                }
            }
            int promoted = promote_next_for_stratum(e, s);
            if (promoted < 0) {
                return engine_err("Failed to promote facts");
            }
            if (promoted > 0) {
                changed = 1;
            }
        } while (changed);
    }

    return engine_ok();
}

EngineRelationView engine_get_relation_view(Engine *e, int pred_id) {
    EngineRelationView view;
    view.pred_id = pred_id;
    view.num_tuples = 0;
    view.tuples = NULL;
    if (!e || pred_id < 0 || pred_id >= e->num_preds) {
        return view;
    }
    PredRuntime *pr = &e->preds[pred_id];
    view.num_tuples = pr->rel.base.size;
    view.tuples = pr->rel.base.items;
    return view;
}
