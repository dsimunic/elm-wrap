#define _DARWIN_C_SOURCE  /* For mkstemps on macOS */
#include "package_fetch.h"
#include "../install_env.h"
#include "../cache.h"
#include "../alloc.h"
#include "../constants.h"
#include "../vendor/cJSON.h"
#include "../vendor/sha1.h"
#include "../log.h"
#include "../http_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ========================================================================
 * Progress Callback State
 * ======================================================================== */

static PackageDownloadProgressCallback g_progress_callback = NULL;
static void *g_progress_user_data = NULL;

/* ========================================================================
 * Path/URL Construction Helpers
 * ======================================================================== */

char* build_package_dir_path(const char *packages_dir, const char *author,
                            const char *name, const char *version) {
    if (!packages_dir || !author || !name || !version) return NULL;

    size_t path_len = strlen(packages_dir) + strlen(author) +
                      strlen(name) + strlen(version) + 4;
    char *path = arena_malloc(path_len);
    if (!path) return NULL;

    snprintf(path, path_len, "%s/%s/%s/%s", packages_dir, author, name, version);
    return path;
}

char* build_package_file_path(const char *packages_dir, const char *author,
                             const char *name, const char *version,
                             const char *filename) {
    if (!packages_dir || !author || !name || !version || !filename) return NULL;

    size_t path_len = strlen(packages_dir) + strlen(author) +
                      strlen(name) + strlen(version) + strlen(filename) + 5;
    char *path = arena_malloc(path_len);
    if (!path) return NULL;

    snprintf(path, path_len, "%s/%s/%s/%s/%s",
             packages_dir, author, name, version, filename);
    return path;
}

char* build_registry_url(const char *registry_base, const char *author,
                        const char *name, const char *version,
                        const char *filename) {
    if (!registry_base || !author || !name || !version || !filename) return NULL;

    size_t url_len = strlen(registry_base) + strlen(author) +
                     strlen(name) + strlen(version) + strlen(filename) + 20;
    char *url = arena_malloc(url_len);
    if (!url) return NULL;

    snprintf(url, url_len, "%s/packages/%s/%s/%s/%s",
             registry_base, author, name, version, filename);
    return url;
}

/* ========================================================================
 * Progress Reporting Functions
 * ======================================================================== */

void package_fetch_set_progress_callback(PackageDownloadProgressCallback callback,
                                        void *user_data) {
    g_progress_callback = callback;
    g_progress_user_data = user_data;
}

void package_fetch_clear_progress_callback(void) {
    g_progress_callback = NULL;
    g_progress_user_data = NULL;
}

static void report_progress(const char *author, const char *name, const char *version,
                          size_t current_bytes, size_t total_bytes)
    __attribute__((unused));

static void report_progress(const char *author, const char *name, const char *version,
                          size_t current_bytes, size_t total_bytes) {
    if (g_progress_callback) {
        g_progress_callback(author, name, version, current_bytes, total_bytes,
                          g_progress_user_data);
    }
}

/* ========================================================================
 * Internal Helper Functions
 * ======================================================================== */

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_string_to_bytes(const char *hex, BYTE *bytes, size_t byte_len) {
    if (!hex || !bytes) return false;

    size_t hex_len = strlen(hex);
    if (hex_len != byte_len * 2) return false;

    for (size_t i = 0; i < byte_len; i++) {
        int high = hex_char_to_int(hex[i * 2]);
        int low = hex_char_to_int(hex[i * 2 + 1]);

        if (high < 0 || low < 0) return false;

        bytes[i] = (BYTE)((high << 4) | low);
    }

    return true;
}

static bool compute_file_sha1(const char *filepath, BYTE hash[SHA1_BLOCK_SIZE]) {
    if (!filepath || !hash) return false;

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file for SHA-1 computation: %s\n", filepath);
        return false;
    }

    SHA1_CTX ctx;
    sha1_init(&ctx);

    BYTE buffer[MAX_PATH_LENGTH];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sha1_update(&ctx, buffer, bytes_read);
    }

    fclose(file);
    sha1_final(&ctx, hash);

    return true;
}

/* ========================================================================
 * Public Utility Functions
 * ======================================================================== */

bool ensure_directory_recursive(const char *path) {
    if (!path || path[0] == '\0') return false;

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    char *path_copy = arena_strdup(path);
    if (!path_copy) return false;

    char *last_slash = strrchr(path_copy, '/');
    if (last_slash && last_slash != path_copy) {
        *last_slash = '\0';
        if (!ensure_directory_recursive(path_copy)) {
            arena_free(path_copy);
            return false;
        }
        *last_slash = '/';
    }

    arena_free(path_copy);

    return mkdir(path, DIR_PERMISSIONS) == 0 || errno == EEXIST;
}

bool verify_file_sha1(const char *filepath, const char *expected_hex) {
    if (!filepath || !expected_hex) return false;

    BYTE actual_hash[SHA1_BLOCK_SIZE];
    if (!compute_file_sha1(filepath, actual_hash)) {
        return false;
    }

    BYTE expected_hash[SHA1_BLOCK_SIZE];
    if (!hex_string_to_bytes(expected_hex, expected_hash, SHA1_BLOCK_SIZE)) {
        fprintf(stderr, "Error: Invalid SHA-1 hex string: %s\n", expected_hex);
        return false;
    }

    if (memcmp(actual_hash, expected_hash, SHA1_BLOCK_SIZE) != 0) {
        fprintf(stderr, "Error: SHA-1 hash mismatch\n");
        fprintf(stderr, "  Expected: %s\n", expected_hex);
        fprintf(stderr, "  Actual:   ");
        for (int i = 0; i < SHA1_BLOCK_SIZE; i++) {
            fprintf(stderr, "%02x", actual_hash[i]);
        }
        fprintf(stderr, "\n");
        return false;
    }

    return true;
}

char* compute_file_sha1_hex(const char *filepath) {
    if (!filepath) return NULL;

    BYTE hash[SHA1_BLOCK_SIZE];
    if (!compute_file_sha1(filepath, hash)) {
        return NULL;
    }

    char *hex = arena_malloc(SHA1_BLOCK_SIZE * 2 + 1);
    if (!hex) return NULL;

    for (int i = 0; i < SHA1_BLOCK_SIZE; i++) {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    hex[SHA1_BLOCK_SIZE * 2] = '\0';

    return hex;
}

/* ========================================================================
 * Package Metadata Functions
 * ======================================================================== */

PackageMetadata* package_metadata_create(void) {
    PackageMetadata *metadata = arena_calloc(1, sizeof(PackageMetadata));
    return metadata;
}

void package_metadata_free(PackageMetadata *metadata) {
    if (!metadata) return;

    arena_free(metadata->endpoint_json);
    arena_free(metadata->elm_json);
    arena_free(metadata->docs_json);
    arena_free(metadata);
}

PackageEndpoint* package_endpoint_parse(const char *endpoint_json) {
    if (!endpoint_json) return NULL;

    cJSON *json = cJSON_Parse(endpoint_json);
    if (!json) {
        fprintf(stderr, "Error: Failed to parse endpoint.json\n");
        return NULL;
    }

    cJSON *url_obj = cJSON_GetObjectItem(json, "url");
    cJSON *hash_obj = cJSON_GetObjectItem(json, "hash");

    if (!url_obj || !cJSON_IsString(url_obj) ||
        !hash_obj || !cJSON_IsString(hash_obj)) {
        fprintf(stderr, "Error: Invalid endpoint.json format\n");
        cJSON_Delete(json);
        return NULL;
    }

    PackageEndpoint *endpoint = arena_calloc(1, sizeof(PackageEndpoint));
    if (!endpoint) {
        cJSON_Delete(json);
        return NULL;
    }

    endpoint->url = arena_strdup(url_obj->valuestring);
    endpoint->hash = arena_strdup(hash_obj->valuestring);

    cJSON_Delete(json);

    if (!endpoint->url || !endpoint->hash) {
        package_endpoint_free(endpoint);
        return NULL;
    }

    return endpoint;
}

void package_endpoint_free(PackageEndpoint *endpoint) {
    if (!endpoint) return;

    arena_free(endpoint->url);
    arena_free(endpoint->hash);
    arena_free(endpoint);
}

bool package_metadata_exists(CacheConfig *config, const char *author,
                             const char *name, const char *version) {
    if (!config || !author || !name || !version) return false;

    char *elm_json_path = build_package_file_path(config->packages_dir, author, name, version, "elm.json");
    char *endpoint_path = build_package_file_path(config->packages_dir, author, name, version, "endpoint.json");
    char *docs_path = build_package_file_path(config->packages_dir, author, name, version, "docs.json");

    if (!elm_json_path || !endpoint_path || !docs_path) {
        arena_free(elm_json_path);
        arena_free(endpoint_path);
        arena_free(docs_path);
        return false;
    }

    struct stat st;
    bool elm_json_exists = (stat(elm_json_path, &st) == 0 && S_ISREG(st.st_mode));
    bool endpoint_exists = (stat(endpoint_path, &st) == 0 && S_ISREG(st.st_mode));
    bool docs_exists = (stat(docs_path, &st) == 0 && S_ISREG(st.st_mode));

    arena_free(elm_json_path);
    arena_free(endpoint_path);
    arena_free(docs_path);

    return elm_json_exists && endpoint_exists && docs_exists;
}

/* ========================================================================
 * Package Fetching Functions
 * ======================================================================== */

bool fetch_package_metadata(InstallEnv *env, const char *author,
                            const char *name, const char *version) {
    if (!env || !author || !name || !version) return false;

    if (env->offline) {
        fprintf(stderr, "Error: Cannot download metadata in offline mode\n");
        return false;
    }

    char *pkg_dir = build_package_dir_path(env->cache->packages_dir, author, name, version);
    if (!pkg_dir) return false;

    if (!ensure_directory_recursive(pkg_dir)) {
        fprintf(stderr, "Error: Failed to create package directory: %s\n", pkg_dir);
        arena_free(pkg_dir);
        return false;
    }

    bool success = true;
    struct stat st;

    const char *files[] = {"endpoint.json", "elm.json", "docs.json"};
    const size_t file_count = sizeof(files) / sizeof(files[0]);

    for (size_t i = 0; i < file_count; i++) {
        const char *filename = files[i];

        char *file_path = build_package_file_path(env->cache->packages_dir, author, name, version, filename);
        if (!file_path) {
            success = false;
            continue;
        }

        if (stat(file_path, &st) == 0 && S_ISREG(st.st_mode)) {
            arena_free(file_path);
            continue;
        }

        char *url = build_registry_url(env->registry_url, author, name, version, filename);
        if (!url) {
            arena_free(file_path);
            success = false;
            continue;
        }

        log_progress("Fetching %s for %s/%s %s...", filename, author, name, version);
        HttpResult result = http_download_file(env->curl_session, url, file_path);

        if (result != HTTP_OK) {
            fprintf(stderr, "Error: Failed to download %s for %s/%s %s: %s\n",
                    filename, author, name, version, http_result_to_string(result));
            success = false;
        }

        arena_free(url);
        arena_free(file_path);
    }

    arena_free(pkg_dir);
    return success;
}

char* fetch_package_archive(InstallEnv *env, const char *author,
                            const char *name, const char *version,
                            PackageEndpoint *endpoint) {
    if (!env || !author || !name || !version || !endpoint) return NULL;

    if (env->offline) {
        fprintf(stderr, "Error: Cannot download package archive in offline mode\n");
        return NULL;
    }

    log_progress("Downloading archive for %s/%s %s...", author, name, version);
    log_progress("  Archive URL: %s", endpoint->url);
    log_progress("  Expected SHA-1: %s", endpoint->hash);

    /* Create unique temporary file for download */
    char temp_template[PATH_MAX];
    snprintf(temp_template, sizeof(temp_template),
             "%s/elm-package-XXXXXX.zip", env->cache->elm_home);

    int temp_fd = mkstemps(temp_template, 4);  /* 4 = strlen(".zip") */
    if (temp_fd == -1) {
        fprintf(stderr, "Error: Failed to create temporary file: %s\n", strerror(errno));
        return NULL;
    }

    /* Close the fd - http_download_file will reopen it */
    close(temp_fd);

    HttpResult result = http_download_file(env->curl_session, endpoint->url, temp_template);
    if (result != HTTP_OK) {
        fprintf(stderr, "Error: Failed to download package archive\n");
        fprintf(stderr, "  URL: %s\n", endpoint->url);
        fprintf(stderr, "  Error: %s\n", http_result_to_string(result));
        remove(temp_template);
        return NULL;
    }

    log_progress("  Downloaded to: %s", temp_template);

    if (env->ignore_hash) {
        log_progress("  Skipping SHA-1 verification (--ignore-hash)");
    } else {
        log_progress("  Verifying SHA-1 hash...");
        if (!verify_file_sha1(temp_template, endpoint->hash)) {
            fprintf(stderr, "Error: SHA-1 verification failed\n");
            remove(temp_template);
            return NULL;
        }
        log_progress("  SHA-1 verification passed");
    }

    /* Return the temporary file path (caller is responsible for cleanup) */
    return arena_strdup(temp_template);
}

char* fetch_package_complete(InstallEnv *env, const char *author,
                             const char *name, const char *version) {
    if (!env || !author || !name || !version) return NULL;

    log_progress("Fetching package %s/%s %s...", author, name, version);

    if (!package_metadata_exists(env->cache, author, name, version)) {
        if (!fetch_package_metadata(env, author, name, version)) {
            fprintf(stderr, "Error: Failed to fetch metadata for %s/%s %s\n",
                    author, name, version);
            return NULL;
        }
    }

    char *endpoint_path = build_package_file_path(env->cache->packages_dir, author, name, version, "endpoint.json");
    if (!endpoint_path) {
        fprintf(stderr, "Error: Failed to build endpoint path\n");
        return NULL;
    }

    FILE *endpoint_file = fopen(endpoint_path, "r");
    if (!endpoint_file) {
        fprintf(stderr, "Error: Cannot open endpoint.json: %s\n", endpoint_path);
        arena_free(endpoint_path);
        return NULL;
    }

    arena_free(endpoint_path);

    fseek(endpoint_file, 0, SEEK_END);
    long file_size = ftell(endpoint_file);
    fseek(endpoint_file, 0, SEEK_SET);

    char *endpoint_data = arena_malloc(file_size + 1);
    if (!endpoint_data) {
        fclose(endpoint_file);
        return NULL;
    }

    size_t read_size = fread(endpoint_data, 1, file_size, endpoint_file);
    endpoint_data[read_size] = '\0';
    fclose(endpoint_file);

    PackageEndpoint *endpoint = package_endpoint_parse(endpoint_data);
    arena_free(endpoint_data);

    if (!endpoint) {
        fprintf(stderr, "Error: Failed to parse endpoint.json\n");
        return NULL;
    }

    char *archive_path = fetch_package_archive(env, author, name, version, endpoint);
    package_endpoint_free(endpoint);

    return archive_path;
}
