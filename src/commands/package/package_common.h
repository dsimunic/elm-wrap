#ifndef PACKAGE_COMMON_H
#define PACKAGE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../solver.h"
#include "../../cache.h"

#define ELM_JSON_PATH "elm.json"

bool parse_package_name(const char *package, char **author, char **name);
Package* find_existing_package(ElmJson *elm_json, const char *author, const char *name);

/**
 * Find which PackageMap contains a package (if any).
 * Works for both applications and packages.
 *
 * @param elm_json   The elm.json to search
 * @param author     Package author
 * @param name       Package name
 * @return The PackageMap containing the package, or NULL if not found
 */
PackageMap* find_package_map(ElmJson *elm_json, const char *author, const char *name);

/**
 * Remove a package from all dependency maps in an application.
 * Only works for application projects; does nothing for package projects.
 *
 * @param elm_json   The elm.json to modify
 * @param author     Package author
 * @param name       Package name
 */
void remove_from_all_app_maps(ElmJson *elm_json, const char *author, const char *name);

bool read_package_info_from_elm_json(const char *elm_json_path, char **out_author, char **out_name, char **out_version);

/**
 * Convert a pinned version (e.g., "1.0.0") to an Elm package constraint
 * (e.g., "1.0.0 <= v < 2.0.0").
 * Returns arena-allocated string, or NULL on failure.
 */
char* version_to_constraint(const char *version);

/**
 * Add or update a package in elm.json with proper version handling.
 *
 * For packages (type=package), converts point versions to constraints.
 * For applications, uses point versions as-is.
 *
 * @param elm_json       The elm.json to modify
 * @param author         Package author
 * @param name           Package name
 * @param version        Version string (will be converted to constraint if needed)
 * @param is_test        Whether this is a test dependency
 * @param is_direct      Whether this is a direct dependency (vs indirect)
 * @param remove_first   If true, remove from all maps before adding (for applications)
 * @return true on success, false on error
 */
bool add_or_update_package_in_elm_json(
    ElmJson *elm_json,
    const char *author,
    const char *name,
    const char *version,
    bool is_test,
    bool is_direct,
    bool remove_first
);

char* find_package_elm_json(const char *pkg_path);
bool install_from_file(const char *source_path, InstallEnv *env, const char *author, const char *name, const char *version);
int compare_package_changes(const void *a, const void *b);

/**
 * Check if a package exists in the registry and count valid versions.
 * Works with both V1 and V2 protocols.
 *
 * @param env Install environment with registry data
 * @param author Package author
 * @param name Package name
 * @param out_version_count Output: number of available versions (can be NULL)
 * @return true if package exists with at least one valid version, false otherwise
 */
bool package_exists_in_registry(InstallEnv *env, const char *author, const char *name,
                                 size_t *out_version_count);

/**
 * Find orphaned indirect dependencies in an application's elm.json.
 *
 * Uses the no_orphaned_packages rulr rule to detect indirect dependencies
 * that are not reachable from any direct dependency.
 *
 * @param elm_json       The application's parsed elm.json
 * @param cache          Cache config for looking up package elm.json files
 * @param exclude_author If non-NULL, exclude this package from direct deps (for simulating removal)
 * @param exclude_name   If non-NULL, exclude this package from direct deps (for simulating removal)
 * @param out_orphaned   Output: PackageMap of orphaned packages (caller must free), or NULL if none
 * @return true on success, false on error
 */
bool find_orphaned_packages(
    const ElmJson *elm_json,
    CacheConfig *cache,
    const char *exclude_author,
    const char *exclude_name,
    PackageMap **out_orphaned
);

#endif /* PACKAGE_COMMON_H */
