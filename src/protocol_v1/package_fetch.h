#ifndef PACKAGE_FETCH_H
#define PACKAGE_FETCH_H

#include <stdbool.h>
#include <stddef.h>

/* Include required types - we need the actual type definitions */
#include "../install_env.h"
#include "../cache.h"

/* Progress callback for download operations
 * Called periodically during download with:
 *   - author/name/version: package being downloaded
 *   - current_bytes: bytes downloaded so far
 *   - total_bytes: total size (0 if unknown)
 *   - user_data: optional user data passed through
 */
typedef void (*PackageDownloadProgressCallback)(
    const char *author,
    const char *name,
    const char *version,
    size_t current_bytes,
    size_t total_bytes,
    void *user_data
);

/* Package metadata structure */
typedef struct {
    char *endpoint_json;  /* Content of endpoint.json */
    char *elm_json;       /* Content of elm.json */
    char *docs_json;      /* Content of docs.json */
} PackageMetadata;

/* Package archive information from endpoint.json */
typedef struct {
    char *url;            /* Archive download URL */
    char *hash;           /* Expected SHA-1 hash */
} PackageEndpoint;

/* Initialize package metadata structure */
PackageMetadata* package_metadata_create(void);

/* Free package metadata structure */
void package_metadata_free(PackageMetadata *metadata);

/* Parse endpoint.json to extract archive URL and hash */
PackageEndpoint* package_endpoint_parse(const char *endpoint_json);

/* Free package endpoint structure */
void package_endpoint_free(PackageEndpoint *endpoint);

/* Check if metadata files exist in cache for a package version */
bool package_metadata_exists(CacheConfig *config, const char *author,
                             const char *name, const char *version);

/* Fetch metadata files (endpoint.json, elm.json, docs.json) from registry
 * Downloads to cache if they don't already exist.
 * Returns true on success.
 */
bool fetch_package_metadata(InstallEnv *env, const char *author,
                            const char *name, const char *version);

/* Download package archive to a temporary file
 * The endpoint parameter should contain the parsed endpoint.json content.
 * Returns the path to the downloaded archive file (caller must free).
 * Verifies SHA-1 hash against expected value.
 */
char* fetch_package_archive(InstallEnv *env, const char *author,
                            const char *name, const char *version,
                            PackageEndpoint *endpoint);

/* Complete package fetch: metadata + archive
 * Downloads metadata if missing, then downloads and verifies archive.
 * Returns path to downloaded archive (caller must free).
 */
char* fetch_package_complete(InstallEnv *env, const char *author,
                             const char *name, const char *version);

/* Utility functions */

/* Ensure directory exists recursively (like mkdir -p) */
bool ensure_directory_recursive(const char *path);

/* Verify SHA-1 hash of a file against expected hex string */
bool verify_file_sha1(const char *filepath, const char *expected_hex);

/* Compute SHA-1 hash of a file and return as hex string (caller must free) */
char* compute_file_sha1_hex(const char *filepath);

/* Path/URL construction helpers */

/* Build package directory path: {cache}/packages/{author}/{name}/{version}
 * Returns allocated string (caller must free) */
char* build_package_dir_path(const char *packages_dir, const char *author,
                            const char *name, const char *version);

/* Build path to a file in the package directory
 * Returns allocated string (caller must free) */
char* build_package_file_path(const char *packages_dir, const char *author,
                             const char *name, const char *version,
                             const char *filename);

/* Build registry URL for a package file
 * Returns allocated string (caller must free) */
char* build_registry_url(const char *registry_base, const char *author,
                        const char *name, const char *version,
                        const char *filename);

/* Progress reporting */

/* Set global progress callback for package downloads */
void package_fetch_set_progress_callback(PackageDownloadProgressCallback callback,
                                        void *user_data);

/* Clear progress callback */
void package_fetch_clear_progress_callback(void);

#endif /* PACKAGE_FETCH_H */
