#ifndef INSTALL_ENV_H
#define INSTALL_ENV_H

#include "cache.h"
#include "registry.h"
#include "http_client.h"
#include <stdbool.h>
#include <stddef.h>

/* Install environment aggregates all resources needed for install */
typedef struct InstallEnv {
    CacheConfig *cache;
    Registry *registry;
    CurlSession *curl_session;
    bool offline;
    char *registry_url;
    char *registry_etag;  /* For future conditional requests */
    size_t known_version_count;
} InstallEnv;

/* Environment lifecycle */
InstallEnv* install_env_create(void);
bool install_env_init(InstallEnv *env);
void install_env_free(InstallEnv *env);

/* Registry operations */
bool install_env_fetch_registry(InstallEnv *env);
bool install_env_update_registry(InstallEnv *env);

/* Package download operations */
bool install_env_download_package(InstallEnv *env, const char *author, const char *name, const char *version);

#endif /* INSTALL_ENV_H */
