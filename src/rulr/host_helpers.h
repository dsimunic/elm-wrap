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

#endif /* RULR_HOST_HELPERS_H */
