#ifndef DEPENDENCY_USAGE_H
#define DEPENDENCY_USAGE_H

#include <stdbool.h>

#include "elm_json.h"

struct CacheConfig;

/**
 * Find production direct dependencies that are not imported by any Elm file
 * in an application's configured source directories.
 *
 * The analysis uses exposed module metadata from the package cache. It fails
 * closed when source files or package metadata cannot be read, so callers do
 * not report partial results as unused dependencies.
 *
 * @param elm_json_path Path to the application's elm.json.
 * @param elm_json Parsed application elm.json.
 * @param cache Package cache used to resolve exposed modules.
 * @param out_unused Output PackageMap, or NULL when no unused dependencies exist.
 * @return true when the complete analysis succeeded, false otherwise.
 */
bool dependency_usage_find_unused_direct(
    const char *elm_json_path,
    const ElmJson *elm_json,
    struct CacheConfig *cache,
    PackageMap **out_unused
);

/**
 * Print the redundant dependency report shared by `wrap info` and
 * `wrap remove --redundant`.
 */
void dependency_usage_report_redundant(
    const PackageMap *unused_direct,
    const PackageMap *orphaned_indirect
);

#endif
