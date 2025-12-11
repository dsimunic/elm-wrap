#ifndef PACKAGE_COMMON_H
#define PACKAGE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../elm_json.h"

#define ELM_JSON_PATH "elm.json"

/* Forward declarations for opaque types used in function signatures */
struct InstallEnv;
struct CacheConfig;

/*
 * Unified version representation.
 * Used across all version parsing, comparison, and formatting operations.
 */
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} Version;

/*
 * Version range bound.
 */
typedef struct {
    Version v;
    bool inclusive;   /* true = >=/<= , false = >/< */
    bool unbounded;   /* true = no bound in this direction */
} VersionBound;

/*
 * Version range for constraints like "1.0.0 <= v < 2.0.0".
 */
typedef struct {
    VersionBound lower;
    VersionBound upper;
    bool is_empty;
} VersionRange;

/*
 * Version parsing and formatting
 */

/* Parse "X.Y.Z" string into Version struct.
 * Returns Version with all zeros if parsing fails.
 * Use version_parse_safe() to check if parsing succeeded for non-zero versions. */
Version version_parse(const char *version_str);

/* Parse "X.Y.Z" string into Version struct with success indicator.
 * Returns true on success, false on failure.
 * Preferred over version_parse() when you need to distinguish 0.0.0 from parse failure. */
bool version_parse_safe(const char *version_str, Version *out);

/* Format Version struct to "X.Y.Z" string.
 * Returns arena-allocated string. Caller should arena_free when done. */
char *version_to_string(const Version *v);

/* Format Version components directly to "X.Y.Z" string.
 * Convenience function when you have separate major/minor/patch values.
 * Returns arena-allocated string. */
char *version_format(uint16_t major, uint16_t minor, uint16_t patch);

/*
 * Version comparison
 */

/* Compare two Version structs.
 * Returns: negative if a < b, zero if a == b, positive if a > b */
int version_compare(const Version *a, const Version *b);

/* Check if two versions are equal. */
bool version_equals(const Version *a, const Version *b);

/*
 * Version constraints
 */

/* Check if a version string is a constraint (contains "<=" or "<") vs exact version. */
bool version_is_constraint(const char *version_str);

/* Parse Elm-style constraint "X.Y.Z <= v < A.B.C" into VersionRange.
 * Returns true on success. For exact versions, both bounds are set to that version. */
bool version_parse_constraint(const char *constraint, VersionRange *out);

/* Check if a version falls within a range. */
bool version_in_range(const Version *v, const VersionRange *range);

/* Format VersionRange to human-readable string.
 * Returns arena-allocated string.
 * Formats: "X.Y.Z" (exact), "^X.Y.Z" (caret), ">=X.Y.Z <A.B.C" (general) */
char *version_range_to_string(const VersionRange *range);

/*
 * Package specification parsing
 */

/* Parse "author/name@version" into components.
 * Returns true on success.
 * out_author, out_name: arena-allocated strings (caller frees)
 * out_version: Version struct */
bool parse_package_with_version(const char *spec, char **out_author, char **out_name, Version *out_version);

/* Parse "author/name@X.Y.Z" keeping version as string.
 * Returns true on success.
 * out_author, out_name, out_version: arena-allocated strings (caller frees) */
bool parse_package_spec(const char *spec, char **out_author, char **out_name, char **out_version);

/*
 * Constraint utilities
 */

/* Create constraint string "X.Y.Z <= v < (X+1).0.0" from exact version.
 * Returns arena-allocated string. */
char *version_to_major_constraint(const char *version);

/* Create exact VersionRange for a single version. */
VersionRange version_range_exact(Version v);

/* Create VersionRange allowing any version within same major (^X.Y.Z). */
VersionRange version_range_until_next_major(Version v);

/* Create VersionRange allowing any version within same minor. */
VersionRange version_range_until_next_minor(Version v);

/* Create unbounded VersionRange (matches any version). */
VersionRange version_range_any(void);

/* Compute intersection of two ranges. */
VersionRange version_range_intersect(VersionRange a, VersionRange b);

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
bool install_from_file(const char *source_path, struct InstallEnv *env, const char *author, const char *name, const char *version);
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
bool package_exists_in_registry(struct InstallEnv *env, const char *author, const char *name,
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
    struct CacheConfig *cache,
    const char *exclude_author,
    const char *exclude_name,
    PackageMap **out_orphaned
);

#endif /* PACKAGE_COMMON_H */
