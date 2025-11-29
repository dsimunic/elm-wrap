#include <stdio.h>
#include <string.h>
#include "ir/ir_builder.h"
#include "common/dyn_array.h"
#include "alloc.h"

static EngineError make_error(const char *msg) {
    EngineError err;
    err.is_error = 1;
    snprintf(err.message, sizeof(err.message), "%s", msg);
    return err;
}

void ir_program_init(IrProgram *prog) {
    prog->pred_table.count = 0;
    prog->num_rules = 0;
    prog->max_stratum = 0;
}

int ir_find_predicate(const PredTable *pt, const char *name) {
    for (int i = 0; i < pt->count; ++i) {
        if (pt->preds[i].name && strcmp(pt->preds[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static EngineArgType parse_type_name(const char *tname) {
    if (tname == NULL) {
        return ARG_TYPE_UNKNOWN;
    }
    if (strcmp(tname, "symbol") == 0) {
        return ARG_TYPE_SYMBOL;
    }
    if (strcmp(tname, "int") == 0) {
        return ARG_TYPE_INT;
    }
    if (strcmp(tname, "range") == 0) {
        return ARG_TYPE_RANGE;
    }
    return ARG_TYPE_UNKNOWN;
}

static EngineError add_predicate(
    PredTable *pt,
    const char *name,
    int arity,
    const EngineArgType *types,
    int declared
) {
    if (pt->count >= MAX_PREDICATES) {
        return make_error("Too many predicates");
    }
    PredDef *pd = &pt->preds[pt->count];
    pd->name = arena_strdup(name);
    pd->arity = arity;
    pd->declared = declared;
    pd->stratum = 0;
    for (int i = 0; i < MAX_ARITY; ++i) {
        pd->arg_types[i] = ARG_TYPE_UNKNOWN;
    }
    if (types) {
        for (int i = 0; i < arity && i < MAX_ARITY; ++i) {
            pd->arg_types[i] = types[i];
        }
    }
    pt->count += 1;
    EngineError ok = {0};
    return ok;
}

EngineError ir_register_predicate(
    PredTable *pt,
    const char *name,
    int arity,
    const EngineArgType *types,
    int declared,
    PredId *out_id
) {
    int idx = ir_find_predicate(pt, name);
    if (idx < 0) {
        EngineError err = add_predicate(pt, name, arity, types, declared);
        if (err.is_error) {
            return err;
        }
        if (out_id) {
            *out_id = pt->count - 1;
        }
        EngineError ok = {0};
        return ok;
    }

    PredDef *pd = &pt->preds[idx];
    if (arity >= 0 && pd->arity >= 0 && pd->arity != arity) {
        return make_error("Predicate arity mismatch");
    }
    if (pd->arity < 0 && arity >= 0) {
        pd->arity = arity;
    }
    if (types) {
        for (int i = 0; i < arity; ++i) {
            if (pd->arg_types[i] == ARG_TYPE_UNKNOWN) {
                pd->arg_types[i] = types[i];
            } else if (types[i] != ARG_TYPE_UNKNOWN && pd->arg_types[i] != types[i]) {
                return make_error("Predicate type mismatch");
            }
        }
    }
    if (declared) {
        pd->declared = 1;
    }
    if (out_id) {
        *out_id = idx;
    }
    EngineError ok = {0};
    return ok;
}

static EngineError validate_fact(const AstFact *fact, PredTable *pt) {
    EngineArgType tmp_types[MAX_ARITY];
    for (int i = 0; i < MAX_ARITY; ++i) {
        tmp_types[i] = ARG_TYPE_UNKNOWN;
    }

    PredId pid;
    EngineError err = ir_register_predicate(pt, fact->pred, fact->arity, tmp_types, 0, &pid);
    if (err.is_error) {
        return err;
    }
    PredDef *pd = &pt->preds[pid];
    if (pd->arity != fact->arity) {
        return make_error("Fact arity does not match predicate");
    }

    for (int i = 0; i < fact->arity; ++i) {
        EngineArgType expected = pd->arg_types[i];
        if (expected == ARG_TYPE_UNKNOWN) {
            continue;
        }
        if (expected == ARG_TYPE_INT || expected == ARG_TYPE_RANGE) {
            if (fact->arg_kind[i] != AST_ARG_INT) {
                return make_error("Fact argument type mismatch");
            }
        } else if (expected == ARG_TYPE_SYMBOL) {
            if (fact->arg_kind[i] != AST_ARG_STRING) {
                return make_error("Fact argument type mismatch");
            }
        }
    }
    EngineError ok = {0};
    return ok;
}

static int find_var(const char *name, char **vars, int count) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(vars[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static EngineError translate_term(
    const AstTerm *term,
    IrTerm *out,
    char **vars,
    int *var_count,
    InternSymbolFn intern,
    void *user_data
) {
    EngineError ok = {0};
    if (term->kind == TERM_INT) {
        out->kind = IR_TERM_INT;
        out->i = term->u.i;
        return ok;
    }
    if (term->kind == TERM_STRING) {
        if (!intern) {
            return make_error("No symbol interner configured");
        }
        int sym_id = intern(user_data, term->u.s);
        if (sym_id < 0) {
            return make_error("Symbol interner failed");
        }
        out->kind = IR_TERM_SYM;
        out->sym = sym_id;
        return ok;
    }
    if (term->kind == TERM_VAR) {
        int idx = find_var(term->u.var.name, vars, *var_count);
        if (idx < 0) {
            if (*var_count >= MAX_VARS) {
                return make_error("Too many variables in rule");
            }
            vars[*var_count] = term->u.var.name;
            idx = *var_count;
            *var_count += 1;
        }
        out->kind = IR_TERM_VAR;
        out->var = idx;
        return ok;
    }
    return make_error("Unknown term kind");
}

static EngineError translate_rule(
    const AstRule *rule,
    IrRule *out,
    PredTable *pt,
    InternSymbolFn intern,
    void *user_data
) {
    EngineArgType unknown[MAX_ARITY];
    for (int i = 0; i < MAX_ARITY; ++i) {
        unknown[i] = ARG_TYPE_UNKNOWN;
    }

    PredId head_id;
    EngineError err = ir_register_predicate(pt, rule->head_pred, rule->head_arity, unknown, 0, &head_id);
    if (err.is_error) {
        return err;
    }
    PredDef *head_pd = &pt->preds[head_id];
    if (head_pd->arity != rule->head_arity) {
        return make_error("Rule head arity mismatch");
    }

    char *vars[MAX_VARS];
    int var_count = 0;
    int var_positive[MAX_VARS] = {0};
    int var_seen[MAX_VARS] = {0};

    out->head_pred = head_id;
    out->head_arity = rule->head_arity;
    out->num_body = 0;
    out->num_vars = 0;

    for (int i = 0; i < rule->head_arity; ++i) {
        IrTerm t;
        err = translate_term(&rule->head_args[i], &t, vars, &var_count, intern, user_data);
        if (err.is_error) {
            return err;
        }
        if (t.kind == IR_TERM_VAR) {
            var_seen[t.var] = 1;
        }
        out->head_args[i] = t;
    }

    for (int i = 0; i < rule->num_body; ++i) {
        if (out->num_body >= MAX_LITERALS) {
            return make_error("Rule has too many body literals");
        }
        AstLiteral *lit = &rule->body[i];
        IrLiteral dest;
        memset(&dest, 0, sizeof(dest));
        dest.kind = IR_LIT_POS;
        dest.arity = lit->arity;

        if (lit->kind == LIT_EQ) {
            dest.kind = IR_LIT_EQ;
            err = translate_term(&lit->lhs, &dest.lhs, vars, &var_count, intern, user_data);
            if (err.is_error) {
                return err;
            }
            err = translate_term(&lit->rhs, &dest.rhs, vars, &var_count, intern, user_data);
            if (err.is_error) {
                return err;
            }
            if (dest.lhs.kind == IR_TERM_VAR) {
                var_seen[dest.lhs.var] = 1;
            }
            if (dest.rhs.kind == IR_TERM_VAR) {
                var_seen[dest.rhs.var] = 1;
            }
            out->body[out->num_body++] = dest;
            continue;
        }

        EngineArgType tmp_types[MAX_ARITY];
        for (int a = 0; a < MAX_ARITY; ++a) {
            tmp_types[a] = ARG_TYPE_UNKNOWN;
        }

        PredId body_pred;
        err = ir_register_predicate(pt, lit->pred, lit->arity, tmp_types, 0, &body_pred);
        if (err.is_error) {
            return err;
        }
        PredDef *body_pd = &pt->preds[body_pred];
        if (body_pd->arity != lit->arity) {
            return make_error("Literal arity mismatch");
        }

        dest.pred = body_pred;
        dest.kind = (lit->kind == LIT_NEG) ? IR_LIT_NEG : IR_LIT_POS;
        dest.arity = lit->arity;
        for (int a = 0; a < lit->arity; ++a) {
            err = translate_term(&lit->args[a], &dest.args[a], vars, &var_count, intern, user_data);
            if (err.is_error) {
                return err;
            }
            if (dest.kind == IR_LIT_POS && dest.args[a].kind == IR_TERM_VAR) {
                var_positive[dest.args[a].var] = 1;
                var_seen[dest.args[a].var] = 1;
            } else if (dest.args[a].kind == IR_TERM_VAR) {
                var_seen[dest.args[a].var] = 1;
            }

            EngineArgType expected = body_pd->arg_types[a];
            if (expected != ARG_TYPE_UNKNOWN) {
                if ((expected == ARG_TYPE_INT || expected == ARG_TYPE_RANGE) && dest.args[a].kind == IR_TERM_SYM) {
                    return make_error("Literal argument type mismatch");
                }
                if (expected == ARG_TYPE_SYMBOL && dest.args[a].kind == IR_TERM_INT) {
                    return make_error("Literal argument type mismatch");
                }
            }
        }
        out->body[out->num_body++] = dest;
    }

    for (int i = 0; i < var_count; ++i) {
        if (var_seen[i] && !var_positive[i]) {
            return make_error("Unsafe rule: variable must appear in a positive literal");
        }
    }

    out->num_vars = var_count;
    EngineError ok = {0};
    return ok;
}

static EngineError compute_strata(IrProgram *prog) {
    int changed = 1;
    int iterations = 0;
    int max_stratum = 0;

    while (changed) {
        changed = 0;
        iterations += 1;
        if (iterations > MAX_PREDICATES + 1) {
            return make_error("Rules are not stratifiable (negation cycle)");
        }

        for (int r = 0; r < prog->num_rules; ++r) {
            IrRule *rule = &prog->rules[r];
            PredDef *head_pd = &prog->pred_table.preds[rule->head_pred];
            int required = head_pd->stratum;
            for (int i = 0; i < rule->num_body; ++i) {
                IrLiteral *lit = &rule->body[i];
                if (lit->kind == IR_LIT_POS) {
                    PredDef *bd = &prog->pred_table.preds[lit->pred];
                    if (bd->stratum > required) {
                        required = bd->stratum;
                    }
                } else if (lit->kind == IR_LIT_NEG) {
                    PredDef *bd = &prog->pred_table.preds[lit->pred];
                    if (bd->stratum + 1 > required) {
                        required = bd->stratum + 1;
                    }
                }
            }
            if (required > head_pd->stratum) {
                head_pd->stratum = required;
                if (required > max_stratum) {
                    max_stratum = required;
                }
                changed = 1;
            }
        }
    }

    prog->max_stratum = max_stratum;
    EngineError ok = {0};
    return ok;
}

EngineError ir_build_from_ast(const AstProgram *ast, IrProgram *prog, InternSymbolFn intern, void *user_data) {
    prog->num_rules = 0;
    prog->max_stratum = 0;

    for (int i = 0; i < prog->pred_table.count; ++i) {
        prog->pred_table.preds[i].stratum = 0;
    }

    for (int i = 0; i < ast->num_decls; ++i) {
        const AstDecl *decl = &ast->decls[i];
        if (decl->arity > MAX_ARITY) {
            return make_error("Predicate arity exceeds MAX_ARITY");
        }
        EngineArgType types[MAX_ARITY];
        for (int a = 0; a < decl->arity; ++a) {
            types[a] = parse_type_name(decl->arg_types[a]);
        }
        PredId pid;
        EngineError err = ir_register_predicate(&prog->pred_table, decl->name, decl->arity, types, 1, &pid);
        if (err.is_error) {
            return err;
        }
    }

    for (int i = 0; i < ast->num_facts; ++i) {
        EngineError err = validate_fact(&ast->facts[i], &prog->pred_table);
        if (err.is_error) {
            return err;
        }
    }

    for (int i = 0; i < ast->num_rules; ++i) {
        if (prog->num_rules >= MAX_RULES) {
            return make_error("Too many rules");
        }
        IrRule *dest = &prog->rules[prog->num_rules];
        EngineError err = translate_rule(&ast->rules[i], dest, &prog->pred_table, intern, user_data);
        if (err.is_error) {
            return err;
        }
        prog->num_rules += 1;
    }

    return compute_strata(prog);
}
