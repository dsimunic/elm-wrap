/**
 * host_helpers.h - Rulr host fact insertion helpers
 *
 * This module provides convenience functions for inserting facts into a Rulr
 * engine from host code. These are commonly used when the host program extracts
 * information from external sources (files, AST, etc.) and needs to inject
 * that data as facts for rule evaluation.
 */

#ifndef RULR_HOST_HELPERS_H
#define RULR_HOST_HELPERS_H

#include "rulr.h"

/**
 * Insert a fact with a single symbol argument.
 *
 * If the predicate doesn't exist, it will be registered automatically.
 *
 * @param r Rulr instance
 * @param pred Predicate name
 * @param s1 Symbol value
 * @return Fact ID on success, -1 on failure
 */
int rulr_insert_fact_1s(Rulr *r, const char *pred, const char *s1);

/**
 * Insert a fact with two symbol arguments.
 *
 * If the predicate doesn't exist, it will be registered automatically.
 *
 * @param r Rulr instance
 * @param pred Predicate name
 * @param s1 First symbol value
 * @param s2 Second symbol value
 * @return Fact ID on success, -1 on failure
 */
int rulr_insert_fact_2s(Rulr *r, const char *pred, const char *s1, const char *s2);

/**
 * Insert a fact with three symbol arguments.
 *
 * If the predicate doesn't exist, it will be registered automatically.
 *
 * @param r Rulr instance
 * @param pred Predicate name
 * @param s1 First symbol value
 * @param s2 Second symbol value
 * @param s3 Third symbol value
 * @return Fact ID on success, -1 on failure
 */
int rulr_insert_fact_3s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3);

/**
 * Insert a fact with four symbol arguments.
 *
 * If the predicate doesn't exist, it will be registered automatically.
 *
 * @param r Rulr instance
 * @param pred Predicate name
 * @param s1 First symbol value
 * @param s2 Second symbol value
 * @param s3 Third symbol value
 * @param s4 Fourth symbol value
 * @return Fact ID on success, -1 on failure
 */
int rulr_insert_fact_4s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3, const char *s4);

/**
 * Insert a fact using a preconstructed array of `Value`s.
 *
 * This generic helper respects `MAX_ARITY` and will reject invalid arities
 * or null `vals` when `arity > 0` so it remains correct if `MAX_ARITY` is
 * increased.
 *
 * @param r Rulr instance
 * @param pred Predicate name
 * @param arity Number of arguments (0..MAX_ARITY)
 * @param vals Array of `Value` of length `arity` or NULL when `arity==0`
 * @return Fact ID on success, -1 on failure
 */
int rulr_insert_fact_vals(Rulr *r, const char *pred, int arity, const Value *vals);

/**
 * Insert a fact with one integer argument.
 */
int rulr_insert_fact_1i(Rulr *r, const char *pred, long i1);

/**
 * Insert a fact with two integer arguments.
 */
int rulr_insert_fact_2i(Rulr *r, const char *pred, long i1, long i2);

/**
 * Insert a fact with one symbol and one integer argument.
 */
int rulr_insert_fact_si(Rulr *r, const char *pred, const char *s1, long i1);

/**
 * Insert a fact with one integer and one symbol argument.
 */
int rulr_insert_fact_is(Rulr *r, const char *pred, long i1, const char *s1);

/**
 * Insert a fact with two integers and one symbol argument (int, int, symbol).
 */
int rulr_insert_fact_2is(Rulr *r, const char *pred, long i1, long i2, const char *s1);

/**
 * Insert a fact with three integer arguments.
 */
int rulr_insert_fact_3i(Rulr *r, const char *pred, long i1, long i2, long i3);

/**
 * Insert a fact with four integer arguments.
 */
int rulr_insert_fact_4i(Rulr *r, const char *pred, long i1, long i2, long i3, long i4);

/**
 * Insert a fact with (int, symbol, int, int) format.
 */
int rulr_insert_fact_isii(Rulr *r, const char *pred, long i1, const char *s, long i2, long i3);

#endif /* RULR_HOST_HELPERS_H */
