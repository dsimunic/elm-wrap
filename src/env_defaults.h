#ifndef ENV_DEFAULTS_H
#define ENV_DEFAULTS_H

/**
 * Environment variable lookup with compiled-in defaults.
 * 
 * These functions check the environment variable first, and if not set,
 * return the default value from the ENV_DEFAULTS file (compiled into the binary).
 * 
 * Returns arena-allocated strings that expand ~ to the user's home directory.
 */

/* Get WRAP_REGISTRY_V2_FULL_INDEX_URL with fallback to compiled default */
char *env_get_registry_v2_full_index_url(void);

/* Get WRAP_REPOSITORY_LOCAL_PATH with fallback to compiled default */
char *env_get_repository_local_path(void);

/* Get WRAP_ELM_COMPILER_PATH (no compiled default, returns NULL if not set) */
char *env_get_elm_compiler_path(void);

#endif /* ENV_DEFAULTS_H */
