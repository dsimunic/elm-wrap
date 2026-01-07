#ifndef BUILDER_H
#define BUILDER_H

#include "../../cache.h"

/**
 * Compile all tracked local-dev packages before main compilation.
 *
 * For application projects: finds all tracked local-dev packages and
 * attempts to compile them silently using --report=json. If any package
 * fails to compile, re-runs without --report=json to provide human-friendly
 * error messages.
 *
 * This ensures local-dev package changes are caught early with clear errors.
 *
 * @param elm_json_path Path to the application's elm.json file
 * @param cache         Cache configuration for looking up package paths
 * @param out_error_output Optional output for human-friendly error message if compilation fails
 * @return true if all packages compile successfully, false otherwise
 */
bool builder_compile_local_dev_packages(
    const char *elm_json_path,
    CacheConfig *cache,
    char **out_error_output
);

/**
 * Clean build artifacts for local-dev packages before compilation.
 *
 * For application projects: finds all tracked local-dev packages and deletes
 * artifacts.dat and artifacts.x.dat from their cache directories.
 *
 * For package projects: deletes artifacts.dat and artifacts.x.dat from the
 * project root directory (same location as elm.json).
 *
 * This ensures that changes to local-dev packages are picked up by the compiler.
 *
 * @param elm_json_path Path to the elm.json file
 * @param cache         Cache configuration for looking up package paths
 * @return true on success (or if no cleanup needed), false on error
 */
bool builder_clean_local_dev_artifacts(const char *elm_json_path, CacheConfig *cache);

#endif /* BUILDER_H */
