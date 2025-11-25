#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>

/* Cache directory structure */
typedef struct {
    char *elm_home;          // Resolved ELM_HOME directory (version-specific root)
    char *elm_version;       // Elm compiler version used for cache paths
    char *packages_dir;      // $ELM_HOME/packages
    char *registry_path;     // $ELM_HOME/packages/registry.dat
} CacheConfig;

/* Initialize cache configuration from environment */
CacheConfig* cache_config_init(void);
void cache_config_free(CacheConfig *config);

/* Package cache operations */
char* cache_get_package_path(CacheConfig *config, const char *author, const char *name, const char *version);
bool cache_package_exists(CacheConfig *config, const char *author, const char *name, const char *version);
bool cache_package_fully_downloaded(CacheConfig *config, const char *author, const char *name, const char *version);
bool cache_package_any_version_exists(CacheConfig *config, const char *author, const char *name);
bool cache_registry_exists(CacheConfig *config);

/* Forward declaration */
struct InstallEnv;

/* Download stubs (to be implemented with libcurl) */
bool cache_download_package(CacheConfig *config, const char *author, const char *name, const char *version);
bool cache_download_package_with_env(struct InstallEnv *env, const char *author, const char *name, const char *version);
bool cache_download_registry(CacheConfig *config);

/* Ensure cache directories exist */
bool cache_ensure_directories(CacheConfig *config);

#endif /* CACHE_H */
