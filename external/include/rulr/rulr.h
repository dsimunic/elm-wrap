#ifndef RULR_H
#define RULR_H

#include "engine/engine.h"
#include "runtime/runtime.h"
#include "frontend/ast.h"
#include "intern/symbol_intern.h"
#include "rulr_host.h"

typedef struct {
    RulrHost      host;    /* Host memory interface (copy) */
    Engine       *engine;
    SymbolIntern  symtab;  /* Hash-based symbol table (O(1) lookup) */
} Rulr;

typedef struct {
    int  is_error;
    char message[256];
} RulrError;

RulrError rulr_ok(void);
RulrError rulr_error(const char *message);

RulrError rulr_init(Rulr *r, const RulrHost *host);
void      rulr_deinit(Rulr *r);

int         rulr_intern_symbol(Rulr *r, const char *s);
const char *rulr_lookup_symbol(const Rulr *r, int sym_id);

RulrError rulr_load_program(Rulr *r, const char *source);
RulrError rulr_evaluate(Rulr *r);

/**
 * Load rules from a pre-parsed AST (used for compiled rule files).
 */
RulrError rulr_load_program_ast(Rulr *r, const AstProgram *ast);

/**
 * Clear derived facts and rules, keeping only the base (injected) facts.
 * This allows loading a new rule file and re-evaluating with the same facts.
 */
void rulr_clear_derived(Rulr *r);

EngineRelationView rulr_get_relation(Rulr *r, const char *pred_name);

/**
 * Look up a tuple by its intern ID (for first-class facts).
 * Returns NULL if the ID is invalid or the tuple doesn't exist.
 */
const struct Tuple *rulr_lookup_tuple(const Rulr *r, uint64_t fact_id);

/**
 * Get the name of a predicate by its ID.
 * Returns NULL if the predicate ID is invalid.
 */
const char *rulr_get_predicate_name(const Rulr *r, int pred_id);

/**
 * Extract predicate ID from a fact intern ID.
 */
int rulr_fact_predicate(uint64_t fact_id);

/**
 * Register a BYODS provider for an external predicate.
 * This is a convenience wrapper around engine_register_byods_provider.
 *
 * @param r Rulr instance
 * @param name Predicate name
 * @param arity Number of arguments
 * @param provider Provider callbacks (iter_all is required)
 * @return Predicate ID on success, -1 on error
 */
int rulr_register_byods_provider(Rulr *r, const char *name, int arity, const ByodsProvider *provider);

/**
 * Get the library version string.
 * Returns a string like "0.8.0" (from VERSION file).
 */
const char *rulr_get_version(void);

/**
 * Get build info with git commit and timestamp.
 * Returns a string like "abc1234 2025-01-08T12:34:56Z" (commit hash + commit date).
 * For dirty builds or non-git builds, returns "unknown".
 */
const char *rulr_get_build_info(void);

#endif /* RULR_H */
