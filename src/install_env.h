#ifndef INSTALL_ENV_H
#define INSTALL_ENV_H

#include "cache.h"
#include "registry.h"
#include "http_client.h"
#include "global_context.h"
#include <stdbool.h>
#include <stddef.h>

/* Include V2Registry type - safe to include as v2_registry.h has no dependencies on install_env.h */
#include "protocol_v2/solver/v2_registry.h"

/* Install environment aggregates all resources needed for install */
typedef struct InstallEnv {
    CacheConfig *cache;

    /* V1-specific fields (only populated in V1 mode) */
    Registry *registry;
    CurlSession *curl_session;
    char *registry_url;
    char *registry_etag;  /* For future conditional requests */
    size_t known_version_count;

    /* V2-specific fields (only populated in V2 mode) */
    V2Registry *v2_registry;

    /* Shared state */
    bool offline;
    bool ignore_hash;  /* Skip SHA-1 verification of downloaded archives */
    ProtocolMode protocol_mode;
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
