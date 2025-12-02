/**
 * v2_registry.h - V2 Registry Index Reader
 *
 * This module provides data structures and functions for reading the V2
 * registry index format. The V2 index contains all dependency information
 * needed by the solver, eliminating the need to access individual package
 * elm.json files from the cache.
 */

#ifndef PROTOCOL_V2_SOLVER_V2_REGISTRY_H
#define PROTOCOL_V2_SOLVER_V2_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Version status in the V2 registry index.
 */
typedef enum {
    V2_STATUS_VALID,
    V2_STATUS_OBSOLETE,
    V2_STATUS_MISSING,
    V2_STATUS_MISSING_DEPS
} V2VersionStatus;

/**
 * A single version dependency in the V2 registry format.
 */
typedef struct {
    char *package_name;     /* Full package name "author/name" */
    char *constraint;       /* Version constraint "1.0.0 <= v < 2.0.0" */
} V2Dependency;

/**
 * A single version of a package in the V2 registry.
 */
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    V2VersionStatus status;
    char *license;
    V2Dependency *dependencies;
    size_t dependency_count;
} V2PackageVersion;

/**
 * A package entry in the V2 registry.
 */
typedef struct {
    char *author;
    char *name;
    V2PackageVersion *versions;
    size_t version_count;
} V2PackageEntry;

/**
 * The V2 registry index containing all packages and their metadata.
 */
typedef struct {
    int format_version;
    char *compiler_name;
    char *compiler_version;
    V2PackageEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
} V2Registry;

/**
 * Load the V2 registry from a zip file.
 * The zip file is expected to contain a single text file with the index.
 *
 * @param zip_path Path to the zip file (e.g., repository/elm/0.19.1/index.dat)
 * @return Pointer to the loaded V2Registry, or NULL on error
 */
V2Registry *v2_registry_load_from_zip(const char *zip_path);

/**
 * Load the V2 registry from a plain text file.
 *
 * @param text_path Path to the text index file
 * @return Pointer to the loaded V2Registry, or NULL on error
 */
V2Registry *v2_registry_load_from_text(const char *text_path);

/**
 * Parse a V2 registry index from a memory buffer.
 *
 * @param data Pointer to the text data
 * @param size Size of the data in bytes
 * @return Pointer to the parsed V2Registry, or NULL on error
 */
V2Registry *v2_registry_parse(const char *data, size_t size);

/**
 * Free a V2Registry and all associated memory.
 *
 * @param registry The registry to free
 */
void v2_registry_free(V2Registry *registry);

/**
 * Find a package entry in the V2 registry.
 *
 * @param registry The registry to search
 * @param author Package author
 * @param name Package name
 * @return Pointer to the entry, or NULL if not found
 */
V2PackageEntry *v2_registry_find(V2Registry *registry, const char *author, const char *name);

/**
 * Find a specific version of a package in the V2 registry.
 *
 * @param registry The registry to search
 * @param author Package author
 * @param name Package name
 * @param major Version major
 * @param minor Version minor
 * @param patch Version patch
 * @return Pointer to the version, or NULL if not found
 */
V2PackageVersion *v2_registry_find_version(V2Registry *registry,
                                           const char *author, const char *name,
                                           uint16_t major, uint16_t minor, uint16_t patch);

#endif /* PROTOCOL_V2_SOLVER_V2_REGISTRY_H */
