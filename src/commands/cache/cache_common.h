#ifndef CACHE_COMMON_H
#define CACHE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include "../../install_env.h"

/**
 * Download a package directly from GitHub and install to cache.
 *
 * Downloads from: https://github.com/AUTHOR/NAME/archive/refs/tags/VERSION.zip
 *
 * This function bypasses the package registry and downloads directly from
 * GitHub's archive endpoint. Use this for faster downloads when you already
 * know the exact version you need.
 *
 * @param env        Install environment (needs curl_session and cache config)
 * @param author     Package author (e.g., "elm")
 * @param name       Package name (e.g., "core")
 * @param version    Package version (e.g., "1.0.0")
 * @param verbose    If true, show detailed progress
 * @param error_out  Buffer to write error message on failure (may be NULL)
 * @param error_size Size of error_out buffer
 * @return true on success, false on failure
 */
bool cache_download_from_github(InstallEnv *env, const char *author,
                                const char *name, const char *version,
                                bool verbose, char *error_out, size_t error_size);

#endif /* CACHE_COMMON_H */
