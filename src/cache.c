#include "cache.h"
#include "install_env.h"
#include "elm_compiler.h"
#include "alloc.h"
#include "constants.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_ELM_VERSION "0.19.1"

/* Platform-specific home directory detection */
static char* get_default_elm_home(const char *elm_version) {
#ifdef _WIN32
    const char *profile = getenv("USERPROFILE");
    if (profile && elm_version) {
        size_t len = strlen(profile) + strlen("\\.elm\\") + strlen(elm_version) + 1;
        char *path = arena_malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "%s\\.elm\\%s", profile, elm_version);
        return path;
    }
#endif

    const char *home = getenv("HOME");
    if (home && elm_version) {
        size_t len = strlen(home) + strlen("/.elm/") + strlen(elm_version) + 1;
        char *path = arena_malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "%s/.elm/%s", home, elm_version);
        return path;
    }

    const char *user = getenv("USER");
    if (user && elm_version) {
#ifdef _WIN32
        size_t len = strlen("C:\\Users\\") + strlen(user) + strlen("\\.elm\\") + strlen(elm_version) + 1;
        char *path = arena_malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "C:\\Users\\%s\\.elm\\%s", user, elm_version);
        return path;
#else
        size_t len = strlen("/home/") + strlen(user) + strlen("/.elm/") + strlen(elm_version) + 1;
        char *path = arena_malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "/home/%s/.elm/%s", user, elm_version);
        return path;
#endif
    }

    /* Fallback to relative path */
    const char *version = elm_version ? elm_version : DEFAULT_ELM_VERSION;
    size_t len = strlen("./.elm/") + strlen(version) + 1;
    char *path = arena_malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "./.elm/%s", version);
    return path;
}

static bool ensure_path_exists(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    char *mutable_path = arena_strdup(path);
    if (!mutable_path) {
        return false;
    }

    bool ok = true;
    size_t len = strlen(mutable_path);
    /* Ensure intermediate directories exist when path contains '/' */
    for (size_t i = 1; i < len && ok; i++) {
        if (mutable_path[i] == '/' || mutable_path[i] == '\\') {
            char saved = mutable_path[i];
            mutable_path[i] = '\0';
            struct stat st;
            if (mutable_path[0] != '\0' &&
                stat(mutable_path, &st) != 0) {
                if (mkdir(mutable_path, DIR_PERMISSIONS) != 0) {
                    perror("Failed to create directory");
                    ok = false;
                }
            }
            mutable_path[i] = saved;
        }
    }

    if (ok) {
        struct stat st;
        if (stat(mutable_path, &st) != 0) {
            if (mkdir(mutable_path, DIR_PERMISSIONS) != 0) {
                perror("Failed to create directory");
                ok = false;
            }
        }
    }

    arena_free(mutable_path);
    return ok;
}

CacheConfig* cache_config_init(void) {
    CacheConfig *config = arena_calloc(1, sizeof(CacheConfig));
    if (!config) return NULL;

    /* Determine Elm version in priority order:
     * 1. ELM_VERSION environment variable
     * 2. Compiler version from running --version
     * 3. DEFAULT_ELM_VERSION constant
     */
    const char *env_version = getenv("ELM_VERSION");
    if (env_version && env_version[0] != '\0') {
        config->elm_version = arena_strdup(env_version);
    } else {
        char *compiler_version = elm_compiler_get_version();
        if (compiler_version) {
            config->elm_version = compiler_version;
        } else {
            config->elm_version = arena_strdup(DEFAULT_ELM_VERSION);
        }
    }

    if (!config->elm_version) {
        cache_config_free(config);
        return NULL;
    }

    /* Get ELM_HOME from environment or use default
     * If ELM_HOME is set, suffix it with the determined version
     */
    const char *env_elm_home = getenv("ELM_HOME");
    if (env_elm_home && env_elm_home[0] != '\0') {
        /* Strip trailing slashes from env_elm_home */
        size_t home_len = strlen(env_elm_home);
        while (home_len > 0 && (env_elm_home[home_len - 1] == '/' || env_elm_home[home_len - 1] == '\\')) {
            home_len--;
        }

        /* Suffix with version: ELM_HOME/version */
        size_t len = home_len + strlen("/") + strlen(config->elm_version) + 1;
        config->elm_home = arena_malloc(len);
        if (!config->elm_home) {
            cache_config_free(config);
            return NULL;
        }
        snprintf(config->elm_home, len, "%.*s/%s", (int)home_len, env_elm_home, config->elm_version);
    } else {
        config->elm_home = get_default_elm_home(config->elm_version);
    }

    if (!config->elm_home) {
        cache_config_free(config);
        return NULL;
    }
    
    // Build paths
    size_t packages_len = strlen(config->elm_home) + strlen("/packages") + 1;
    config->packages_dir = arena_malloc(packages_len);
    if (!config->packages_dir) {
        cache_config_free(config);
        return NULL;
    }
    snprintf(config->packages_dir, packages_len, "%s/packages", config->elm_home);
    
    size_t registry_len = strlen(config->packages_dir) + strlen("/registry.dat") + 1;
    config->registry_path = arena_malloc(registry_len);
    if (!config->registry_path) {
        cache_config_free(config);
        return NULL;
    }
    snprintf(config->registry_path, registry_len, "%s/registry.dat", config->packages_dir);
    
    return config;
}

void cache_config_free(CacheConfig *config) {
    if (!config) return;
    
    arena_free(config->elm_version);
    arena_free(config->elm_home);
    arena_free(config->packages_dir);
    arena_free(config->registry_path);
    arena_free(config);
}

char* cache_get_package_path(CacheConfig *config, const char *author, const char *name, const char *version) {
    if (!config || !author || !name || !version) return NULL;
    
    size_t len = strlen(config->packages_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *path = arena_malloc(len);
    if (!path) return NULL;
    
    snprintf(path, len, "%s/%s/%s/%s", config->packages_dir, author, name, version);
    return path;
}

bool cache_package_exists(CacheConfig *config, const char *author, const char *name, const char *version) {
    char *path = cache_get_package_path(config, author, name, version);
    if (!path) return false;

    struct stat st;
    bool exists = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    arena_free(path);
    return exists;
}

bool cache_package_fully_downloaded(CacheConfig *config, const char *author, const char *name, const char *version) {
    if (!config || !author || !name || !version) return false;

    char *pkg_path = cache_get_package_path(config, author, name, version);
    if (!pkg_path) return false;

    log_debug("Checking if package is fully downloaded: %s", pkg_path);

    /* Check if package directory exists */
    struct stat st;
    if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        log_debug("Package directory does not exist: %s", pkg_path);
        arena_free(pkg_path);
        return false;
    }

    log_debug("Package directory exists: %s", pkg_path);

    /* Check if src/ directory exists */
    size_t src_len = strlen(pkg_path) + strlen("/src") + 1;
    char *src_path = arena_malloc(src_len);
    if (!src_path) {
        arena_free(pkg_path);
        return false;
    }

    snprintf(src_path, src_len, "%s/src", pkg_path);
    bool has_src = (stat(src_path, &st) == 0 && S_ISDIR(st.st_mode));

    if (has_src) {
        log_debug("Package src/ directory exists: %s", src_path);
    } else {
        log_debug("Package src/ directory MISSING: %s (package incomplete!)", src_path);
    }

    arena_free(pkg_path);
    arena_free(src_path);
    return has_src;
}

bool cache_registry_exists(CacheConfig *config) {
    if (!config) return false;
    
    struct stat st;
    return (stat(config->registry_path, &st) == 0 && S_ISREG(st.st_mode));
}

bool cache_ensure_directories(CacheConfig *config) {
    if (!config) return false;
    
    if (!ensure_path_exists(config->elm_home)) {
        return false;
    }
    if (!ensure_path_exists(config->packages_dir)) {
        return false;
    }
    return true;
}

/* Download package using InstallEnv (actual implementation) */
bool cache_download_package_with_env(struct InstallEnv *env, const char *author, const char *name, const char *version) {
    if (!env) {
        fprintf(stderr, "Error: InstallEnv is NULL in cache_download_package_with_env\n");
        return false;
    }

    /* Delegate to install_env_download_package which has the real implementation */
    return install_env_download_package(env, author, name, version);
}

bool cache_package_any_version_exists(CacheConfig *config, const char *author, const char *name) {
    if (!config || !author || !name) {
        return false;
    }

    size_t len = strlen(config->packages_dir) + strlen(author) + strlen(name) + 3;
    char *path = arena_malloc(len);
    if (!path) return false;

    snprintf(path, len, "%s/%s/%s", config->packages_dir, author, name);

    struct stat st;
    bool exists = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    arena_free(path);
    return exists;
}
