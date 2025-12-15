#ifndef ENV_DEFAULTS_H
#define ENV_DEFAULTS_H

#include <stdbool.h>

/**
 * Environment variable lookup with compiled-in defaults.
 * 
 * These functions check the environment variable first, and if not set,
 * return the default value from the ENV_DEFAULTS file (compiled into the binary).
 * 
 * Returns arena-allocated strings that expand ~ to the user's home directory.
 */

/* Get WRAP_HOME with fallback to compiled default (base directory for all wrap data) */
char *env_get_wrap_home(void);

/* Get WRAP_REGISTRY_V2_FULL_INDEX_URL with fallback to compiled default */
char *env_get_registry_v2_full_index_url(void);

/* Get full repository path: WRAP_HOME/WRAP_REPOSITORY_LOCAL_PATH */
char *env_get_repository_local_path(void);

/* Get WRAP_ELM_COMPILER_PATH (no compiled default, returns NULL if not set) */
char *env_get_elm_compiler_path(void);

/* Check if offline mode is forced via WRAP_OFFLINE_MODE=1 */
bool env_get_offline_mode(void);

/* Check if registry updates should be skipped via WRAP_SKIP_REGISTRY_UPDATE=1
 * This allows online operations (e.g., downloading packages) while skipping
 * the incremental registry update check. Useful for testing with a pre-populated
 * registry. */
bool env_get_skip_registry_update(void);

#endif /* ENV_DEFAULTS_H */
