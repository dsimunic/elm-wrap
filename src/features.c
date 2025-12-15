#include "feature_flags.h"
#include <stdlib.h>
#include <string.h>

/*
 * Compile-time defaults are set via Makefile CFLAGS.
 * Provide fallback values if not defined.
 */
#ifndef FEATURE_PUBLISH_DEFAULT
#define FEATURE_PUBLISH_DEFAULT 0
#endif

#ifndef FEATURE_REVIEW_DEFAULT
#define FEATURE_REVIEW_DEFAULT 0
#endif

#ifndef FEATURE_POLICY_DEFAULT
#define FEATURE_POLICY_DEFAULT 0
#endif

#ifndef FEATURE_CACHE_DEFAULT
#define FEATURE_CACHE_DEFAULT 0
#endif

/*
 * Check environment variable for feature flag override.
 * Returns: 1 if enabled, 0 if disabled, -1 if not set (use default).
 */
static int check_env_flag(const char *env_var) {
    const char *value = getenv(env_var);
    if (value == NULL) {
        return -1; /* Not set, use default */
    }
    if (strcmp(value, "1") == 0) {
        return 1; /* Enabled */
    }
    if (strcmp(value, "0") == 0) {
        return 0; /* Disabled */
    }
    return -1; /* Invalid value, use default */
}

bool feature_publish_enabled(void) {
    int env_value = check_env_flag("WRAP_FEATURE_PUBLISH");
    if (env_value >= 0) {
        return env_value == 1;
    }
    return FEATURE_PUBLISH_DEFAULT != 0;
}

bool feature_review_enabled(void) {
    int env_value = check_env_flag("WRAP_FEATURE_REVIEW");
    if (env_value >= 0) {
        return env_value == 1;
    }
    return FEATURE_REVIEW_DEFAULT != 0;
}

bool feature_policy_enabled(void) {
    int env_value = check_env_flag("WRAP_FEATURE_POLICY");
    if (env_value >= 0) {
        return env_value == 1;
    }
    return FEATURE_POLICY_DEFAULT != 0;
}

bool feature_cache_enabled(void) {
    int env_value = check_env_flag("WRAP_FEATURE_CACHE");
    if (env_value >= 0) {
        return env_value == 1;
    }
    return FEATURE_CACHE_DEFAULT != 0;
}
