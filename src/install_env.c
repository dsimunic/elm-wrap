#include "install_env.h"
#include "alloc.h"
#include "constants.h"
#include "env_defaults.h"
#include "vendor/cJSON.h"
#include "vendor/sha1.h"
#include "log.h"
#include "fileutil.h"
#include "global_context.h"
#include "registry.h"
#include "protocol_v1/package_fetch.h"
#include "protocol_v2/solver/v2_registry.h"
#include "commands/package/package_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>

char *install_env_registry_etag_file_path(const char *registry_dat_path) {
    if (!registry_dat_path) return NULL;

    size_t len = strlen(registry_dat_path) + strlen(".etag") + 1;
    char *path = arena_malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s.etag", registry_dat_path);
    return path;
}

static void trim_trailing_ws_in_place(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            s[len - 1] = '\0';
            len--;
            continue;
        }
        break;
    }
}

static char *registry_etag_read_from_disk(const char *etag_path) {
    if (!etag_path) return NULL;
    char *contents = file_read_contents_bounded(etag_path, MAX_LARGE_BUFFER_LENGTH, NULL);
    if (!contents) return NULL;
    trim_trailing_ws_in_place(contents);
    if (contents[0] == '\0') {
        arena_free(contents);
        return NULL;
    }
    return contents;
}

static bool registry_etag_write_to_disk(const char *etag_path, const char *etag) {
    if (!etag_path || !etag || etag[0] == '\0') return false;

    char tmp_path[MAX_TEMP_PATH_LENGTH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", etag_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        log_debug("Failed to open %s for writing: %s", tmp_path, strerror(errno));
        return false;
    }

    size_t len = strlen(etag);
    if (fwrite(etag, 1, len, f) != len || fputc('\n', f) == EOF) {
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, etag_path) != 0) {
        log_debug("Failed to rename %s to %s: %s", tmp_path, etag_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }

    return true;
}

char *install_env_registry_since_count_file_path(const char *registry_dat_path) {
    if (!registry_dat_path) return NULL;

    size_t len = strlen(registry_dat_path) + strlen(".since-count") + 1;
    char *path = arena_malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s.since-count", registry_dat_path);
    return path;
}

static bool registry_since_count_read_from_disk(const char *since_path, size_t *out_since_count) {
    if (!since_path || !out_since_count) return false;

    char *contents = file_read_contents_bounded(since_path, MAX_TEMP_BUFFER_LENGTH, NULL);
    if (!contents) return false;

    trim_trailing_ws_in_place(contents);

    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(contents, &end, 10);
    if (errno != 0 || !end || end == contents || *end != '\0') {
        arena_free(contents);
        return false;
    }

    if (val > (unsigned long long)SIZE_MAX) {
        arena_free(contents);
        return false;
    }

    *out_since_count = (size_t)val;
    arena_free(contents);
    return true;
}

static bool registry_since_count_write_to_disk(const char *since_path, size_t since_count) {
    if (!since_path) return false;

    char tmp_path[MAX_TEMP_PATH_LENGTH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", since_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        log_debug("Failed to open %s for writing: %s", tmp_path, strerror(errno));
        return false;
    }

    if (fprintf(f, "%zu\n", since_count) < 0) {
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp_path, since_path) != 0) {
        log_debug("Failed to rename %s to %s: %s", tmp_path, since_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }

    return true;
}

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

    size_t canonical_since_count = 0;

    cJSON *package = NULL;
    cJSON_ArrayForEach(package, json) {
        if (!cJSON_IsArray(package)) continue;

        int pkg_versions = cJSON_GetArraySize(package);
        if (pkg_versions > 0) {
            size_t add = (size_t)pkg_versions;
            if (canonical_since_count > SIZE_MAX - add) {
                fprintf(stderr, "Error: Registry since_count overflow while parsing all-packages JSON\n");
                cJSON_Delete(json);
                return false;
            }
            canonical_since_count += add;
        }

        const char *package_name = package->string;
        if (!package_name) continue;

        /* Parse author/name */
        char *author = NULL;
        char *name = NULL;
        if (!parse_package_name(package_name, &author, &name)) {
            continue;
        }

        registry_add_entry(registry, author, name);

        cJSON *version_item = NULL;
        cJSON_ArrayForEach(version_item, package) {
            if (!cJSON_IsString(version_item)) continue;

            const char *version_str = version_item->valuestring;
            Version version = version_parse(version_str);

            /* Add version maintains descending order */
            if (!registry_add_version_ex(registry, author, name, version, false, NULL)) {
                arena_free(author);
                arena_free(name);
                cJSON_Delete(json);
                return false;
            }
        }

        arena_free(author);
        arena_free(name);
    }

    registry->since_count = canonical_since_count;

    /* Ensure registry is sorted */
    registry_sort_entries(registry);

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

        char *author = NULL;
        char *name = NULL;
        Version version;
        if (!parse_package_with_version(entry_str, &author, &name, &version)) {
            continue;
        }

        if (!registry_add_version_ex(registry, author, name, version, false, NULL)) {
            arena_free(author);
            arena_free(name);
            cJSON_Delete(json);
            return false;
        }

        arena_free(author);
        arena_free(name);
    }

    size_t add = (size_t)count;
    if (registry->since_count > SIZE_MAX - add) {
        cJSON_Delete(json);
        return false;
    }
    registry->since_count += add;

    cJSON_Delete(json);
    return true;
}

static bool install_env_init_cache_common(InstallEnv *env) {
    if (!env) return false;

    bool forced_offline = env_get_offline_mode();
    env->offline_forced = forced_offline;
    if (forced_offline) {
        log_progress("WRAP_OFFLINE_MODE=1: Network operations disabled");
    }

    env->cache = cache_config_init();
    if (!env->cache) {
        fprintf(stderr, "Error: Failed to initialize cache configuration\n");
        return false;
    }

    if (!cache_ensure_directories(env->cache)) {
        fprintf(stderr, "Error: Failed to create cache directories\n");
        return false;
    }

    return true;
}

static bool install_env_init_v1_resources(InstallEnv *env) {
    if (!env) return false;

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

    return true;
}

static void install_env_v1_probe_offline(InstallEnv *env) {
    if (!env || !env->registry_url || !env->curl_session) return;

    char health_check_url[URL_MAX];
    snprintf(health_check_url, sizeof(health_check_url), "%s/all-packages", env->registry_url);

    if (env->offline_forced) {
        log_progress("Offline mode forced via WRAP_OFFLINE_MODE=1");
        env->offline = true;
        return;
    }

    log_progress("Testing connectivity to %s...", env->registry_url);
    env->offline = !curl_session_can_connect(env->curl_session, health_check_url);
}

bool install_env_prepare_v1(InstallEnv *env) {
    if (!env) return false;

    env->protocol_mode = PROTOCOL_V1;
    env->v2_registry = NULL;

    if (!install_env_init_cache_common(env)) {
        return false;
    }

    log_progress("Using V1 protocol mode");

    if (!install_env_init_v1_resources(env)) {
        return false;
    }

    install_env_v1_probe_offline(env);
    return true;
}

bool install_env_ensure_v1_registry(InstallEnv *env) {
    if (!env || !env->cache || !env->curl_session || !env->registry_url) return false;

    env->protocol_mode = PROTOCOL_V1;
    env->v2_registry = NULL;

    if (env->registry) {
        registry_free(env->registry);
        env->registry = NULL;
    }
    if (env->registry_etag) {
        arena_free(env->registry_etag);
        env->registry_etag = NULL;
    }

    env->registry = registry_load_from_dat(env->cache->registry_path, &env->known_version_count);

    /* Load cached ETag (best-effort) */
    char *etag_path = install_env_registry_etag_file_path(env->cache->registry_path);
    if (etag_path) {
        env->registry_etag = registry_etag_read_from_disk(etag_path);
        arena_free(etag_path);
    }

    if (env->registry) {
        log_progress("Loaded cached registry: %zu packages, %zu versions",
                     env->registry->entry_count, env->registry->since_count);

        /* Merge local-dev registry if it exists */
        /* Build path for registry-local-dev.dat next to registry.dat */
        char *reg_dir = arena_strdup(env->cache->registry_path);
        if (reg_dir) {
            char *last_slash = strrchr(reg_dir, '/');
            if (last_slash) {
                *last_slash = '\0';
                size_t local_dev_path_len = strlen(reg_dir) + strlen("/registry-local-dev.dat") + 1;
                char *local_dev_path = arena_malloc(local_dev_path_len);
                if (local_dev_path) {
                    snprintf(local_dev_path, local_dev_path_len, "%s/registry-local-dev.dat", reg_dir);
                    if (!registry_merge_local_dev(env->registry, local_dev_path)) {
                        log_progress("Warning: Failed to merge local-dev registry");
                    }
                    arena_free(local_dev_path);
                }
            }
            arena_free(reg_dir);
        }

        /* Repair inflated/incorrect header since_count using the persisted canonical sidecar (Option B). */
        char *since_path = install_env_registry_since_count_file_path(env->cache->registry_path);
        if (since_path && file_exists(since_path)) {
            size_t canonical_since = 0;
            if (registry_since_count_read_from_disk(since_path, &canonical_since)) {
                if (canonical_since != env->registry->since_count) {
                    fprintf(stderr,
                            "Warning: registry.dat header since_count (%zu) differs from canonical since_count (%zu); repairing header.\n",
                            env->registry->since_count, canonical_since);
                    env->registry->since_count = canonical_since;
                    env->known_version_count = canonical_since;

                    if (!registry_dat_write(env->registry, env->cache->registry_path)) {
                        fprintf(stderr, "Warning: Failed to rewrite registry.dat header for since_count repair\n");
                    }
                }
            } else {
                log_debug("Failed to parse since_count sidecar file: %s", since_path);
            }
        }
        if (since_path) {
            arena_free(since_path);
        }
    } else {
        log_progress("No cached registry found, will fetch from network");
        env->registry = registry_create();
        if (!env->registry) {
            fprintf(stderr, "Error: Failed to create registry\n");
            return false;
        }
        env->known_version_count = 0;
    }

    if (env->offline) {
        if (env->offline_forced) {
            log_progress("Using cached registry (offline mode forced)");
        } else {
            log_progress("Warning: Cannot connect to package registry (offline mode)");
        }

        if (env->known_version_count == 0) {
            fprintf(stderr, "Error: No cached registry and offline mode is active\n");
            if (env->offline_forced) {
                fprintf(stderr, "Hint: Unset WRAP_OFFLINE_MODE or run online first to cache registry data\n");
            } else {
                fprintf(stderr, "Please run again when online to download package registry\n");
            }
            return false;
        }

        log_progress("Using cached registry data");
        return true;
    }

    log_progress("Connected to package registry");

    if (env->known_version_count == 0) {
        if (!install_env_fetch_registry(env)) {
            fprintf(stderr, "Error: Failed to fetch registry from network\n");
            return false;
        }
    } else {
        /* Skip incremental registry update if WRAP_SKIP_REGISTRY_UPDATE=1.
         * This allows online operations (downloading packages) while using
         * a pre-populated registry without contacting the upstream /since endpoint. */
        if (env_get_skip_registry_update()) {
            log_progress("Skipping registry update (WRAP_SKIP_REGISTRY_UPDATE=1)");
        } else if (!install_env_update_registry(env)) {
            fprintf(stderr, "Warning: Failed to update registry (using cached data)\n");
        }
    }

    return true;
}

InstallEnv* install_env_create(void) {
    InstallEnv *env = arena_calloc(1, sizeof(InstallEnv));
    return env;
}

bool install_env_init(InstallEnv *env) {
    if (!env) return false;

    /* Initialize cache configuration (shared by both protocols) */
    if (!install_env_init_cache_common(env)) {
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
            fprintf(stderr, "Hint: Run '%s repository new' to initialize the repository\n", global_context_program_name());
            return false;
        }

        /* Merge local-dev registry if it exists */
        size_t local_dev_path_len = strlen(ctx->repository_path) + strlen("/registry-local-dev.dat") + 1;
        char *local_dev_path = arena_malloc(local_dev_path_len);
        if (local_dev_path) {
            snprintf(local_dev_path, local_dev_path_len, "%s/registry-local-dev.dat", ctx->repository_path);
            if (!v2_registry_merge_local_dev(env->v2_registry, local_dev_path)) {
                log_progress("Warning: Failed to merge local-dev registry");
                /* Continue anyway - main registry is loaded */
            }
            arena_free(local_dev_path);
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
        if (!install_env_init_v1_resources(env)) {
            return false;
        }

        install_env_v1_probe_offline(env);

        if (!install_env_ensure_v1_registry(env)) {
            return false;
        }
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

    char *new_etag = NULL;
    bool not_modified = false;
    HttpResult result = http_get_json_etag(env->curl_session, url, env->registry_etag, buffer, &new_etag, &not_modified);

    if (result != HTTP_OK) {
        fprintf(stderr, "Error: Failed to fetch registry\n");
        fprintf(stderr, "  URL: %s\n", url);
        fprintf(stderr, "  Error: %s\n", http_result_to_string(result));
        fprintf(stderr, "  Details: %s\n", curl_session_get_error(env->curl_session));

        memory_buffer_free(buffer);
        env->offline = true;
        return false;
    }

    if (not_modified) {
        /* Should not generally happen on first fetch, but treat as success */
        log_progress("Registry not modified (ETag match)");
        memory_buffer_free(buffer);
        if (new_etag) {
            if (env->registry_etag) arena_free(env->registry_etag);
            env->registry_etag = new_etag;
            char *etag_path = install_env_registry_etag_file_path(env->cache->registry_path);
            if (etag_path) {
                registry_etag_write_to_disk(etag_path, env->registry_etag);
                arena_free(etag_path);
            }
        }
        return true;
    }

    printf("Downloaded %zu bytes\n", buffer->len);

    if (!parse_all_packages_json(buffer->data, env->registry)) {
        fprintf(stderr, "Error: Failed to parse registry JSON\n");
        memory_buffer_free(buffer);
        return false;
    }

    memory_buffer_free(buffer);

    printf("Registry loaded: %zu packages, %zu versions\n",
           env->registry->entry_count, env->registry->since_count);

    registry_sort_entries(env->registry);
    if (!registry_dat_write(env->registry, env->cache->registry_path)) {
        fprintf(stderr, "Warning: Failed to cache registry to %s\n", env->cache->registry_path);
    } else {
        printf("Registry cached to %s\n", env->cache->registry_path);
        env->known_version_count = env->registry->since_count;

        char *since_path = install_env_registry_since_count_file_path(env->cache->registry_path);
        if (since_path) {
            registry_since_count_write_to_disk(since_path, env->known_version_count);
            arena_free(since_path);
        }
    }

    if (new_etag) {
        if (env->registry_etag) {
            arena_free(env->registry_etag);
        }
        env->registry_etag = new_etag;
        char *etag_path = install_env_registry_etag_file_path(env->cache->registry_path);
        if (etag_path) {
            registry_etag_write_to_disk(etag_path, env->registry_etag);
            arena_free(etag_path);
        }
    }

    return true;
}

bool install_env_update_registry(InstallEnv *env) {
    if (!env || !env->curl_session || !env->registry_url) return false;

    /* If we have an ETag, do a quick HEAD check to avoid /since when unchanged */
    if (env->registry_etag && env->registry_etag[0] != '\0') {
        char all_url[URL_MAX];
        snprintf(all_url, sizeof(all_url), "%s/all-packages", env->registry_url);
        char *head_etag = NULL;
        bool not_modified = false;
        HttpResult head_res = http_head_etag(env->curl_session, all_url, env->registry_etag, &head_etag, &not_modified);
        if (head_res == HTTP_OK && not_modified) {
            log_progress("Registry is up to date (ETag match)");
            if (head_etag) {
                /* Some servers include ETag on 304; keep it */
                arena_free(env->registry_etag);
                env->registry_etag = head_etag;
                char *etag_path = install_env_registry_etag_file_path(env->cache->registry_path);
                if (etag_path) {
                    registry_etag_write_to_disk(etag_path, env->registry_etag);
                    arena_free(etag_path);
                }
            }
            return true;
        }

        if (head_res == HTTP_OK && head_etag) {
            /* Keep new ETag; will write after successful update */
            if (env->registry_etag) {
                arena_free(env->registry_etag);
            }
            env->registry_etag = head_etag;
        }
    }

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

    size_t new_total = env->registry->since_count;
    if (new_total > env->known_version_count) {
        log_progress("Registry updated: %zu new version(s)", new_total - env->known_version_count);

        registry_sort_entries(env->registry);
        if (!registry_dat_write(env->registry, env->cache->registry_path)) {
            fprintf(stderr, "Warning: Failed to cache updated registry\n");
        } else {
            env->known_version_count = new_total;

            char *since_path = install_env_registry_since_count_file_path(env->cache->registry_path);
            if (since_path) {
                registry_since_count_write_to_disk(since_path, env->known_version_count);
                arena_free(since_path);
            }
        }
    } else {
        log_progress("Registry is up to date");
    }

    /* Persist ETag from latest HEAD check (best-effort) */
    if (env->registry_etag && env->registry_etag[0] != '\0') {
        char *etag_path = install_env_registry_etag_file_path(env->cache->registry_path);
        if (etag_path) {
            registry_etag_write_to_disk(etag_path, env->registry_etag);
            arena_free(etag_path);
        }
    }

    return true;
}

bool install_env_download_package(InstallEnv *env, const char *author, const char *name, const char *version) {
    if (!env || !author || !name || !version) return false;

    if (env->protocol_mode == PROTOCOL_V2) {
        GlobalContext *ctx = global_context_get();
        if (!ctx || !ctx->repository_path) {
            fprintf(stderr, "Error: V2 repository path is not configured\n");
            return false;
        }

        size_t src_path_len = strlen(ctx->repository_path) + strlen("/packages/") +
                              strlen(author) + 1 + strlen(name) + 1 + strlen(version) + 1;
        char *src_path = arena_malloc(src_path_len);
        if (!src_path) {
            fprintf(stderr, "Error: Failed to allocate repository package path\n");
            return false;
        }
        snprintf(src_path, src_path_len, "%s/packages/%s/%s/%s", ctx->repository_path, author, name, version);

        struct stat st;
        if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Error: Package %s/%s %s not found in repository at %s\n",
                    author, name, version, src_path);
            arena_free(src_path);
            return false;
        }

        char *pkg_dir = build_package_dir_path(env->cache->packages_dir, author, name, version);
        if (!pkg_dir) {
            fprintf(stderr, "Error: Failed to allocate package directory path\n");
            arena_free(src_path);
            return false;
        }

        if (strcmp(src_path, pkg_dir) == 0) {
            log_progress("Package %s/%s %s already present in repository cache", author, name, version);
            arena_free(src_path);
            arena_free(pkg_dir);
            return true;
        }

        log_progress("Installing %s/%s %s from repository...", author, name, version);
        bool copy_ok = copy_directory_selective(src_path, pkg_dir);

        if (!copy_ok) {
            fprintf(stderr, "Error: Failed to copy package from repository (%s -> %s)\n",
                    src_path, pkg_dir);
        } else {
            log_progress("  Copied package from repository");
        }

        arena_free(src_path);
        arena_free(pkg_dir);
        return copy_ok;
    }

    if (env->offline) {
        if (env->offline_forced) {
            fprintf(stderr, "Error: Cannot download package while WRAP_OFFLINE_MODE=1 is set\n");
        } else {
            fprintf(stderr, "Error: Cannot download package in offline mode\n");
        }
        return false;
    }

    log_progress("Downloading %s/%s %s...", author, name, version);

    char *archive_path = fetch_package_complete(env, author, name, version);
    if (!archive_path) {
        fprintf(stderr, "Error: Failed to fetch package %s/%s %s\n", author, name, version);
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

    log_progress("  Successfully installed %s/%s %s", author, name, version);

    return true;
}

bool install_env_solver_online(const InstallEnv *env) {
    if (!env) return true;  /* Default to online if no env */
    return !env->offline;
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
