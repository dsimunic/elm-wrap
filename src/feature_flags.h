#ifndef FEATURE_FLAGS_H
#define FEATURE_FLAGS_H

#include <stdbool.h>

/*
 * Feature flags for hiding development commands from regular users.
 *
 * Compile-time defaults are set via Makefile (FEATURE_CODE, FEATURE_PUBLISH,
 * FEATURE_REVIEW, FEATURE_POLICY, FEATURE_CACHE). Runtime environment variables can override:
 *   - WRAP_FEATURE_CODE: "1" to enable, "0" to disable
 *   - WRAP_FEATURE_PUBLISH: "1" to enable, "0" to disable
 *   - WRAP_FEATURE_REVIEW: "1" to enable, "0" to disable
 *   - WRAP_FEATURE_POLICY: "1" to enable, "0" to disable
 *   - WRAP_FEATURE_CACHE: "1" to enable, "0" to disable
 */

/* Check if the 'code' command group is enabled */
bool feature_code_enabled(void);

/* Check if the 'publish' command (deprecated wrapper and package publish) is enabled */
bool feature_publish_enabled(void);

/* Check if the 'review' command group is enabled */
bool feature_review_enabled(void);

/* Check if the 'policy' command group is enabled */
bool feature_policy_enabled(void);

/* Check if the 'package cache' subcommand is enabled */
bool feature_cache_enabled(void);

#endif /* FEATURE_FLAGS_H */
