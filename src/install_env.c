#include "install_env.h"
#include "alloc.h"
#include "cJSON.h"
#include "vendor/sha1.h"
#include "log.h"
#include "fileutil.h"
#include "global_context.h"
#include "protocol_v1/package_fetch.h"
#include "protocol_v2/solver/v2_registry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Maximum URL length - should be sufficient for registry URLs */
#define URL_MAX 512

#define DEFAULT_REGISTRY_URL "https://package.elm-lang.org"

/* Compile all-packages.json into Registry.dat */
static bool parse_all_packages_json(const char *json_str, Registry *registry) {
    if (!json_str || !registry) return false;

    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        fprintf(stderr, "Error: Failed to parse all-packages JSON\n");
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            /* Show context around error */
            const char *start = (error_ptr > json_str + 40) ? error_ptr - 40 : json_str;
            const char *end = error_ptr + 40;
            fprintf(stderr, "Error near: ...%.*s...\n", (int)(end - start), start);
        }
        return false;
    }

    registry->total_versions = 0;

    cJSON *package = NULL;
    cJSON_ArrayForEach(package, json) {
        if (!cJSON_IsArray(package)) continue;

        const char *package_name = package->string;
        if (!package_name) continue;

        /* Parse author/name */
        const char *slash = strchr(package_name, '/');
        if (!slash) continue;

        size_t author_len = slash - package_name;
        char *author = arena_malloc(author_len + 1);
        if (!author) {
            cJSON_Delete(json);
            return false;
        }

        strncpy(author, package_name, author_len);
        author[author_len] = '\0';
        const char *name = slash + 1;

        registry_add_entry(registry, author, name);

        cJSON *version_item = NULL;
        cJSON_ArrayForEach(version_item, package) {
            if (!cJSON_IsString(version_item)) continue;

            const char *version_str = version_item->valuestring;
            Version version = version_parse(version_str);

            /* Add version maintains descending order */
            registry_add_version(registry, author, name, version);
        }

        arena_free(author);
    }

    cJSON_Delete(json);
    return true;
}

/* Parse incremental update response (array of "author/package@version" strings) */
static bool parse_since_response(const char *json_str, Registry *registry) {
    if (!json_str || !registry) return false;

    cJSON *json = cJSON_Parse(json_str);
    if (!json || !cJSON_IsArray(json)) {
        fprintf(stderr, "Error: Failed to parse /since response JSON\n");
        if (json) cJSON_Delete(json);
        return false;
    }

    int count = cJSON_GetArraySize(json);
    if (count == 0) {
        cJSON_Delete(json);
        return true;
    }

    printf("Received %d new package version(s)\n", count);

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (!cJSON_IsString(item)) continue;

        const char *entry_str = item->valuestring;

        const char *at = strchr(entry_str, '@');
        if (!at) continue;

        size_t package_name_len = at - entry_str;
        char *package_name = arena_malloc(package_name_len + 1);
        if (!package_name) {
            cJSON_Delete(json);
            return false;
        }

        strncpy(package_name, entry_str, package_name_len);
        package_name[package_name_len] = '\0';

        const char *slash = strchr(package_name, '/');
        if (!slash) {
            arena_free(package_name);
            continue;
        }

        size_t author_len = slash - package_name;
        char *author = arena_malloc(author_len + 1);
        if (!author) {
            arena_free(package_name);
            cJSON_Delete(json);
            return false;
        }

        strncpy(author, package_name, author_len);
        author[author_len] = '\0';
        const char *name = slash + 1;

        const char *version_str = at + 1;
        Version version = version_parse(version_str);

        registry_add_version(registry, author, name, version);

        arena_free(author);
        arena_free(package_name);
    }

    cJSON_Delete(json);
    return true;
}

InstallEnv* install_env_create(void) {
    InstallEnv *env = arena_calloc(1, sizeof(InstallEnv));
    return env;
}

bool install_env_init(InstallEnv *env) {
    if (!env) return false;

    /* Initialize cache configuration (shared by both protocols) */
    env->cache = cache_config_init();
    if (!env->cache) {
        fprintf(stderr, "Error: Failed to initialize cache configuration\n");
        return false;
    }

    if (!cache_ensure_directories(env->cache)) {
        fprintf(stderr, "Error: Failed to create cache directories\n");
        return false;
    }

    /* Determine protocol mode */
    if (global_context_is_v2()) {
        env->protocol_mode = PROTOCOL_V2;
        log_progress("Using V2 protocol mode");

        /* Load V2 registry from local index */
        GlobalContext *ctx = global_context_get();
        if (!ctx || !ctx->repository_path) {
            fprintf(stderr, "Error: V2 mode active but no repository path available\n");
            return false;
        }

        size_t index_path_len = strlen(ctx->repository_path) + strlen("/index.dat") + 1;
        char *index_path = arena_malloc(index_path_len);
        if (!index_path) {
            fprintf(stderr, "Error: Failed to allocate index path\n");
            return false;
        }

        snprintf(index_path, index_path_len, "%s/index.dat", ctx->repository_path);
        log_progress("Loading V2 registry from %s", index_path);

        env->v2_registry = v2_registry_load_from_zip(index_path);
        arena_free(index_path);

        if (!env->v2_registry) {
            fprintf(stderr, "Error: Failed to load V2 registry from %s/index.dat\n", ctx->repository_path);
            fprintf(stderr, "Hint: Run 'elm-wrap repository new' to initialize the repository\n");
            return false;
        }

        log_progress("V2 registry loaded successfully");

        /* V2 mode is always "online" since registry is local */
        env->offline = false;

        /* V1-specific fields remain NULL/uninitialized */
        env->curl_session = NULL;
        env->registry = NULL;
        env->registry_url = NULL;
        env->registry_etag = NULL;
        env->known_version_count = 0;

    } else {
        env->protocol_mode = PROTOCOL_V1;
        log_progress("Using V1 protocol mode");

        /* Initialize V1-specific resources */
        env->curl_session = curl_session_create();
        if (!env->curl_session) {
            fprintf(stderr, "Error: Failed to initialize HTTP client\n");
            return false;
        }

        const char *registry_url_env = getenv("ELM_PACKAGE_REGISTRY_URL");
        if (registry_url_env && registry_url_env[0] != '\0') {
            env->registry_url = arena_strdup(registry_url_env);
        } else {
            env->registry_url = arena_strdup(DEFAULT_REGISTRY_URL);
        }

        if (!env->registry_url) {
            fprintf(stderr, "Error: Failed to allocate registry URL\n");
            return false;
        }

        env->registry = registry_load_from_dat(env->cache->registry_path, &env->known_version_count);

        if (env->registry) {
            log_progress("Loaded cached registry: %zu packages, %zu versions",
                   env->registry->entry_count, env->registry->total_versions);
        } else {
            log_progress("No cached registry found, will fetch from network");
            env->registry = registry_create();
            if (!env->registry) {
                fprintf(stderr, "Error: Failed to create registry\n");
                return false;
            }
            env->known_version_count = 0;
        }

        char health_check_url[URL_MAX];
        snprintf(health_check_url, sizeof(health_check_url), "%s/all-packages", env->registry_url);

        log_progress("Testing connectivity to %s...", env->registry_url);
        env->offline = !curl_session_can_connect(env->curl_session, health_check_url);

        if (env->offline) {
            log_progress("Warning: Cannot connect to package registry (offline mode)");

            if (env->known_version_count == 0) {
                fprintf(stderr, "Error: No cached registry and cannot connect to network\n");
                fprintf(stderr, "Please run again when online to download package registry\n");
                return false;
            }

            log_progress("Using cached registry data");
        } else {
            log_progress("Connected to package registry");

            if (env->known_version_count == 0) {
                if (!install_env_fetch_registry(env)) {
                    fprintf(stderr, "Error: Failed to fetch registry from network\n");
                    return false;
                }
            } else {
                if (!install_env_update_registry(env)) {
                    fprintf(stderr, "Warning: Failed to update registry (using cached data)\n");
                }
            }
        }

        /* V2-specific fields remain NULL/uninitialized */
        env->v2_registry = NULL;
    }

    return true;
}

bool install_env_fetch_registry(InstallEnv *env) {
    if (!env || !env->curl_session || !env->registry_url) return false;

    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s/all-packages", env->registry_url);

    printf("Fetching package registry from %s...\n", url);

    MemoryBuffer *buffer = memory_buffer_create();
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate memory buffer\n");
        return false;
    }

    HttpResult result = http_get_json(env->curl_session, url, buffer);

    if (result != HTTP_OK) {
        fprintf(stderr, "Error: Failed to fetch registry\n");
        fprintf(stderr, "  URL: %s\n", url);
        fprintf(stderr, "  Error: %s\n", http_result_to_string(result));
        fprintf(stderr, "  Details: %s\n", curl_session_get_error(env->curl_session));

        memory_buffer_free(buffer);
        env->offline = true;
        return false;
    }

    printf("Downloaded %zu bytes\n", buffer->len);

    if (!parse_all_packages_json(buffer->data, env->registry)) {
        fprintf(stderr, "Error: Failed to parse registry JSON\n");
        memory_buffer_free(buffer);
        return false;
    }

    memory_buffer_free(buffer);

    printf("Registry loaded: %zu packages, %zu versions\n",
           env->registry->entry_count, env->registry->total_versions);

    if (!registry_dat_write(env->registry, env->cache->registry_path)) {
        fprintf(stderr, "Warning: Failed to cache registry to %s\n", env->cache->registry_path);
    } else {
        printf("Registry cached to %s\n", env->cache->registry_path);
        env->known_version_count = env->registry->total_versions;
    }

    return true;
}

bool install_env_update_registry(InstallEnv *env) {
    if (!env || !env->curl_session || !env->registry_url) return false;

    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s/all-packages/since/%zu",
             env->registry_url, env->known_version_count);

    log_progress("Checking for registry updates (known: %zu versions)...", env->known_version_count);

    MemoryBuffer *buffer = memory_buffer_create();
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate memory buffer\n");
        return false;
    }

    HttpResult result = http_get_json(env->curl_session, url, buffer);

    if (result != HTTP_OK) {
        fprintf(stderr, "Warning: Failed to fetch registry updates\n");
        fprintf(stderr, "  URL: %s\n", url);
        fprintf(stderr, "  Error: %s\n", http_result_to_string(result));

        memory_buffer_free(buffer);
        return false;
    }

    if (!parse_since_response(buffer->data, env->registry)) {
        fprintf(stderr, "Warning: Failed to parse registry update\n");
        memory_buffer_free(buffer);
        return false;
    }

    memory_buffer_free(buffer);

    size_t new_total = env->registry->total_versions;
    if (new_total > env->known_version_count) {
        log_progress("Registry updated: %zu new version(s)", new_total - env->known_version_count);

        if (!registry_dat_write(env->registry, env->cache->registry_path)) {
            fprintf(stderr, "Warning: Failed to cache updated registry\n");
        } else {
            env->known_version_count = new_total;
        }
    } else {
        log_progress("Registry is up to date");
    }

    return true;
}

bool install_env_download_package(InstallEnv *env, const char *author, const char *name, const char *version) {
    if (!env || !author || !name || !version) return false;

    if (env->offline) {
        fprintf(stderr, "Error: Cannot download package in offline mode\n");
        return false;
    }

    log_progress("Downloading %s/%s@%s...", author, name, version);

    char *archive_path = fetch_package_complete(env, author, name, version);
    if (!archive_path) {
        fprintf(stderr, "Error: Failed to fetch package %s/%s@%s\n", author, name, version);
        return false;
    }

    char *pkg_dir = build_package_dir_path(env->cache->packages_dir, author, name, version);
    if (!pkg_dir) {
        fprintf(stderr, "Error: Failed to allocate package directory path\n");
        remove(archive_path);
        arena_free(archive_path);
        return false;
    }

    if (!ensure_directory_recursive(pkg_dir)) {
        fprintf(stderr, "Error: Failed to create package directory: %s\n", pkg_dir);
        arena_free(pkg_dir);
        remove(archive_path);
        arena_free(archive_path);
        return false;
    }

    log_progress("  Extracting to: %s", pkg_dir);

    /* Only extracts elm.json, docs.json, LICENSE, README.md, and src/
     * Automatically handles GitHub zipball structure (skips leading directory component)
     * Won't overwrite existing elm.json or docs.json
     */
    if (!extract_zip_selective(archive_path, pkg_dir)) {
        fprintf(stderr, "Error: Failed to extract package archive\n");
        arena_free(pkg_dir);
        remove(archive_path);
        arena_free(archive_path);
        return false;
    }

    remove(archive_path);
    arena_free(archive_path);
    arena_free(pkg_dir);

    log_progress("  Successfully installed %s/%s@%s", author, name, version);

    return true;
}

void install_env_free(InstallEnv *env) {
    if (!env) return;

    cache_config_free(env->cache);

    /* Free V1-specific resources */
    if (env->registry) {
        registry_free(env->registry);
    }
    if (env->curl_session) {
        curl_session_free(env->curl_session);
    }
    if (env->registry_url) {
        arena_free(env->registry_url);
    }
    if (env->registry_etag) {
        arena_free(env->registry_etag);
    }

    /* Free V2-specific resources */
    if (env->v2_registry) {
        v2_registry_free(env->v2_registry);
    }

    arena_free(env);
}
