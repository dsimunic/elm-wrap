#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "frontend/lexer.h"
#include "frontend/ast.h"
#include "common/dyn_array.h"
#include "alloc.h"

typedef struct {
    Lexer      lx;
    Token      current;
    ParseError err;
    AstProgram *prog;
} Parser;

static void parser_error(Parser *p, const char *fmt, ...) {
    if (p->err.is_error) {
        return;
    }
    p->err.is_error = 1;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->err.message, sizeof(p->err.message), fmt, args);
    va_end(args);
}

static void parser_advance(Parser *p) {
    p->current = lexer_next(&p->lx);
}

static int token_is_upper_ident(Token t) {
    return t.kind == TOK_IDENT && t.lexeme && t.length > 0 && isupper((unsigned char)t.lexeme[0]);
}

static int token_is_lower_ident(Token t) {
    return t.kind == TOK_IDENT && t.lexeme && t.length > 0 && islower((unsigned char)t.lexeme[0]);
}

static char *copy_token_text(Token t) {
    char *buf = (char *)arena_malloc((size_t)t.length + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, t.lexeme, (size_t)t.length);
    buf[t.length] = '\0';
    return buf;
}

static int expect(Parser *p, TokenKind kind, const char *what) {
    if (p->current.kind != kind) {
        parser_error(p, "Expected %s", what);
        return -1;
    }
    parser_advance(p);
    return 0;
}

void ast_program_init(AstProgram *prog) {
    prog->decls = NULL;
    prog->num_decls = 0;
    prog->decls_capacity = 0;
    prog->facts = NULL;
    prog->num_facts = 0;
    prog->facts_capacity = 0;
    prog->rules = NULL;
    prog->num_rules = 0;
    prog->rules_capacity = 0;
    prog->clear_derived = 0;
}

void ast_program_reset(AstProgram *prog) {
    prog->num_decls = 0;
    prog->num_facts = 0;
    prog->num_rules = 0;
    prog->clear_derived = 0;
}

static int parse_arg_decl_list(Parser *p, AstDecl *decl) {
    int idx = 0;
    if (p->current.kind == TOK_RPAREN) {
        return 0;
    }
    while (1) {
        if (idx >= MAX_ARITY) {
            parser_error(p, "Too many predicate arguments");
            return -1;
        }
        if (p->current.kind != TOK_IDENT) {
            parser_error(p, "Expected argument name");
            return -1;
        }
        decl->arg_names[idx] = copy_token_text(p->current);
        parser_advance(p);
        if (expect(p, TOK_COLON, "':'") < 0) {
            return -1;
        }
        if (p->current.kind != TOK_IDENT) {
            parser_error(p, "Expected type name");
            return -1;
        }
        decl->arg_types[idx] = copy_token_text(p->current);
        parser_advance(p);
        idx += 1;
        if (p->current.kind == TOK_COMMA) {
            parser_advance(p);
            continue;
        }
        if (p->current.kind == TOK_RPAREN) {
            break;
        }
        parser_error(p, "Expected ',' or ')'");
        return -1;
    }
    decl->arity = idx;
    return 0;
}

static ParseError parse_decl(Parser *p) {
    AstDecl decl;
    decl.name = NULL;
    decl.arity = 0;
    decl.arg_names = NULL;
    decl.arg_types = NULL;

    parser_advance(p); /* consume TOK_PRED */
    if (p->current.kind != TOK_IDENT) {
        parser_error(p, "Expected predicate name after .pred");
        return p->err;
    }
    decl.name = copy_token_text(p->current);
    parser_advance(p);
    if (expect(p, TOK_LPAREN, "'('") < 0) {
        return p->err;
    }

    decl.arg_names = (char **)arena_calloc(MAX_ARITY, sizeof(char *));
    decl.arg_types = (char **)arena_calloc(MAX_ARITY, sizeof(char *));
    if (!decl.arg_names || !decl.arg_types) {
        parser_error(p, "Out of memory");
        return p->err;
    }
    if (parse_arg_decl_list(p, &decl) < 0) {
        return p->err;
    }
    if (expect(p, TOK_RPAREN, "')'") < 0) {
        return p->err;
    }
    if (expect(p, TOK_DOT, "'.'") < 0) {
        return p->err;
    }

    if (DYNARRAY_PUSH(p->prog->decls, p->prog->num_decls, p->prog->decls_capacity, decl) < 0) {
        parser_error(p, "Out of memory");
        return p->err;
    }
    return p->err;
}

static AstTerm parse_term(Parser *p) {
    AstTerm term;
    term.kind = TERM_VAR;
    term.u.var.id = -1;
    term.u.var.name = NULL;

    if (p->current.kind == TOK_STRING) {
        term.kind = TERM_STRING;
        term.u.s = p->current.string_value;
        parser_advance(p);
        return term;
    }
    if (p->current.kind == TOK_INT) {
        term.kind = TERM_INT;
        term.u.i = p->current.int_value;
        parser_advance(p);
        return term;
    }
    if (p->current.kind == TOK_WILDCARD) {
        term.kind = TERM_WILDCARD;
        parser_advance(p);
        return term;
    }
    if (p->current.kind == TOK_IDENT) {
        if (!token_is_upper_ident(p->current)) {
            parser_error(p, "Expected variable (capitalized) or literal");
            return term;
        }
        term.kind = TERM_VAR;
        term.u.var.name = copy_token_text(p->current);
        parser_advance(p);
        return term;
    }

    parser_error(p, "Expected term");
    return term;
}

static int is_comparison_token(TokenKind kind) {
    return kind == TOK_EQ || kind == TOK_NE || kind == TOK_LT || 
           kind == TOK_LE || kind == TOK_GT || kind == TOK_GE;
}

static AstCmpOp token_to_cmp_op(TokenKind kind) {
    switch (kind) {
        case TOK_EQ: return CMP_EQ;
        case TOK_NE: return CMP_NE;
        case TOK_LT: return CMP_LT;
        case TOK_LE: return CMP_LE;
        case TOK_GT: return CMP_GT;
        case TOK_GE: return CMP_GE;
        default: return CMP_EQ;
    }
}

static int parse_rule_body(Parser *p, AstRule *rule) {
    while (1) {
        if (rule->num_body >= MAX_LITERALS) {
            parser_error(p, "Too many literals in rule body");
            return -1;
        }
        AstLiteral lit;
        memset(&lit, 0, sizeof(lit));

        int is_not = 0;
        if (p->current.kind == TOK_NOT) {
            is_not = 1;
            parser_advance(p);
        }

        if (p->current.kind == TOK_IDENT && token_is_lower_ident(p->current)) {
            char *pred_name = copy_token_text(p->current);
            
            /* Check for 'match' builtin */
            if (strcmp(pred_name, "match") == 0) {
                if (is_not) {
                    parser_error(p, "'not' cannot be used with match builtin");
                    return -1;
                }
                parser_advance(p);
                if (expect(p, TOK_LPAREN, "'('") < 0) {
                    return -1;
                }
                /* match(pattern, string) */
                lit.lhs = parse_term(p);  /* pattern */
                if (p->err.is_error) {
                    return -1;
                }
                if (expect(p, TOK_COMMA, "','") < 0) {
                    return -1;
                }
                lit.rhs = parse_term(p);  /* string to match */
                if (p->err.is_error) {
                    return -1;
                }
                if (expect(p, TOK_RPAREN, "')'") < 0) {
                    return -1;
                }
                lit.kind = LIT_BUILTIN;
                lit.builtin = BUILTIN_MATCH;
                lit.arity = 0;
            } else {
                lit.pred = pred_name;
                lit.kind = is_not ? LIT_NEG : LIT_POS;
                parser_advance(p);
                if (expect(p, TOK_LPAREN, "'('") < 0) {
                    return -1;
                }
                int arg_idx = 0;
                if (p->current.kind != TOK_RPAREN) {
                    while (1) {
                        if (arg_idx >= MAX_ARITY) {
                            parser_error(p, "Too many arguments in literal");
                            return -1;
                        }
                        lit.args[arg_idx] = parse_term(p);
                        if (p->err.is_error) {
                            return -1;
                        }
                        arg_idx += 1;
                        if (p->current.kind == TOK_COMMA) {
                            parser_advance(p);
                            continue;
                        }
                        if (p->current.kind == TOK_RPAREN) {
                            break;
                        }
                        parser_error(p, "Expected ',' or ')' in literal");
                        return -1;
                    }
                }
                lit.arity = arg_idx;
                parser_advance(p); /* consume ) */
            }
        } else {
            AstTerm lhs = parse_term(p);
            if (p->err.is_error) {
                return -1;
            }
            if (!is_comparison_token(p->current.kind)) {
                parser_error(p, "Expected comparison operator (=, !=, <, <=, >, >=)");
                return -1;
            }
            TokenKind cmp_tok = p->current.kind;
            parser_advance(p);
            AstTerm rhs = parse_term(p);
            if (p->err.is_error) {
                return -1;
            }
            if (cmp_tok == TOK_EQ) {
                lit.kind = LIT_EQ;
            } else {
                lit.kind = LIT_CMP;
                lit.cmp_op = token_to_cmp_op(cmp_tok);
            }
            lit.lhs = lhs;
            lit.rhs = rhs;
            lit.arity = 0;
        }

        if (is_not && lit.kind != LIT_NEG) {
            parser_error(p, "'not' must precede a predicate literal");
            return -1;
        }

        if (DYNARRAY_PUSH(rule->body, rule->num_body, rule->body_capacity, lit) < 0) {
            parser_error(p, "Out of memory");
            return -1;
        }

        if (p->current.kind == TOK_COMMA) {
            parser_advance(p);
            continue;
        }
        if (p->current.kind == TOK_DOT) {
            parser_advance(p);
            break;
        }
        parser_error(p, "Expected ',' or '.' after literal");
        return -1;
    }
    return 0;
}

static ParseError parse_fact_or_rule(Parser *p) {
    Token pred_tok = p->current;
    if (!token_is_lower_ident(pred_tok)) {
        parser_error(p, "Predicate names must start with lowercase");
        return p->err;
    }
    char *pred_name = copy_token_text(pred_tok);
    parser_advance(p);
    if (expect(p, TOK_LPAREN, "'('") < 0) {
        return p->err;
    }

    AstTerm head_terms[MAX_ARITY];
    int head_arity = 0;
    if (p->current.kind != TOK_RPAREN) {
        while (1) {
            if (head_arity >= MAX_ARITY) {
                parser_error(p, "Too many head arguments");
                return p->err;
            }
            head_terms[head_arity] = parse_term(p);
            if (p->err.is_error) {
                return p->err;
            }
            head_arity += 1;
            if (p->current.kind == TOK_COMMA) {
                parser_advance(p);
                continue;
            }
            if (p->current.kind == TOK_RPAREN) {
                break;
            }
            parser_error(p, "Expected ',' or ')' in argument list");
            return p->err;
        }
    }
    parser_advance(p); /* consume ) */

    if (p->current.kind == TOK_DOT) {
        parser_advance(p);
        AstFact fact;
        fact.pred = pred_name;
        fact.arity = head_arity;
        for (int i = 0; i < head_arity; ++i) {
            if (head_terms[i].kind == TERM_STRING) {
                fact.arg_kind[i] = AST_ARG_STRING;
                fact.arg_value[i].s = head_terms[i].u.s;
            } else if (head_terms[i].kind == TERM_INT) {
                fact.arg_kind[i] = AST_ARG_INT;
                fact.arg_value[i].i = head_terms[i].u.i;
            } else {
                parser_error(p, "Facts cannot use variables");
                return p->err;
            }
        }
        if (DYNARRAY_PUSH(p->prog->facts, p->prog->num_facts, p->prog->facts_capacity, fact) < 0) {
            parser_error(p, "Out of memory");
            return p->err;
        }
        return p->err;
    }

    if (p->current.kind == TOK_ARROW) {
        parser_advance(p);
        AstRule rule;
        memset(&rule, 0, sizeof(rule));
        rule.head_pred = pred_name;
        rule.head_arity = head_arity;
        for (int i = 0; i < head_arity; ++i) {
            rule.head_args[i] = head_terms[i];
        }
        rule.body_capacity = 0;
        rule.num_body = 0;
        rule.body = NULL;
        if (parse_rule_body(p, &rule) < 0) {
            return p->err;
        }
        if (DYNARRAY_PUSH(p->prog->rules, p->prog->num_rules, p->prog->rules_capacity, rule) < 0) {
            parser_error(p, "Out of memory");
            return p->err;
        }
        return p->err;
    }

    parser_error(p, "Expected '.' or ':-' after head");
    return p->err;
}

ParseError parse_program(const char *source, AstProgram *prog) {
    Parser p;
    p.prog = prog;
    ast_program_reset(prog);
    p.err.is_error = 0;
    p.err.message[0] = '\0';

    lexer_init(&p.lx, source);
    parser_advance(&p);

    while (p.current.kind != TOK_EOF && !p.err.is_error) {
        if (p.current.kind == TOK_PRED) {
            parse_decl(&p);
        } else if (p.current.kind == TOK_CLEAR_DERIVED) {
            /* .clear_derived() directive */
            parser_advance(&p);
            if (expect(&p, TOK_LPAREN, "'('") < 0) {
                break;
            }
            if (expect(&p, TOK_RPAREN, "')'") < 0) {
                break;
            }
            prog->clear_derived = 1;
        } else if (p.current.kind == TOK_IDENT) {
            parse_fact_or_rule(&p);
        } else {
            parser_error(&p, "Unexpected token");
            break;
        }
    }

    return p.err;
}
