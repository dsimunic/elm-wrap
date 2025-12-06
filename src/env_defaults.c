#include "env_defaults.h"
#include "buildinfo.h"
#include "alloc.h"
#include <stdlib.h>
#include <string.h>

/* Expand ~ to the user's home directory in a path */
static char *expand_tilde(const char *path) {
    if (!path || path[0] == '\0') {
        return arena_strdup("");
    }
    
    if (path[0] != '~') {
        return arena_strdup(path);
    }
    
    /* Handle ~/... or just ~ */
    if (path[1] == '\0' || path[1] == '/') {
        const char *home = getenv("HOME");
        if (!home) {
            /* If HOME is not set, return path as-is */
            return arena_strdup(path);
        }
        
        size_t home_len = strlen(home);
        size_t path_len = strlen(path);
        size_t result_len = home_len + path_len; /* -1 for ~ but +1 for nul */
        
        char *result = arena_malloc(result_len);
        if (!result) {
            return NULL;
        }
        
        strcpy(result, home);
        strcat(result, path + 1); /* Skip the ~ */
        return result;
    }
    
    /* ~user/... format is not supported, return as-is */
    return arena_strdup(path);
}

char *env_get_registry_v2_full_index_url(void) {
    const char *env_val = getenv("WRAP_REGISTRY_V2_FULL_INDEX_URL");
    if (env_val && env_val[0] != '\0') {
        return arena_strdup(env_val);
    }
    return arena_strdup(env_default_registry_v2_full_index_url);
}

char *env_get_repository_local_path(void) {
    const char *env_val = getenv("WRAP_REPOSITORY_LOCAL_PATH");
    if (env_val && env_val[0] != '\0') {
        return expand_tilde(env_val);
    }
    return expand_tilde(env_default_repository_local_path);
}

char *env_get_elm_compiler_path(void) {
    const char *env_val = getenv("WRAP_ELM_COMPILER_PATH");
    if (env_val && env_val[0] != '\0') {
        return expand_tilde(env_val);
    }
    return NULL;
}
