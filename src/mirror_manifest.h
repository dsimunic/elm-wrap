#ifndef MIRROR_MANIFEST_H
#define MIRROR_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Mirror manifest for content-addressable package storage.
 *
 * Tracks package versions and their SHA1 hashes for deduplication.
 * Output format: JSON manifest file mapping packages to archive hashes.
 */

/* Single version entry within a package */
typedef struct {
    char *version;  /* e.g., "1.0.0" */
    char *hash;     /* SHA1 hash of the archive */
    char *url;      /* Original download URL */
} MirrorVersionEntry;

/* Package entry with multiple versions */
typedef struct {
    char *author;
    char *name;
    MirrorVersionEntry *versions;
    size_t version_count;
    size_t version_capacity;
} MirrorPackageEntry;

/* Main manifest structure */
typedef struct {
    char *generated;        /* ISO 8601 timestamp */
    char *source;           /* e.g., "package.elm-lang.org" */
    MirrorPackageEntry *packages;
    size_t package_count;
    size_t package_capacity;
} MirrorManifest;

/* Manifest lifecycle */
MirrorManifest* mirror_manifest_create(void);
void mirror_manifest_free(MirrorManifest *m);

/* Add a package version to the manifest
 * Returns true on success, false on allocation failure */
bool mirror_manifest_add(MirrorManifest *m, const char *author, const char *name,
                         const char *version, const char *hash, const char *url);

/* Look up a package version's hash
 * Returns the hash string if found, NULL otherwise */
const char* mirror_manifest_lookup(MirrorManifest *m, const char *author,
                                   const char *name, const char *version);

/* Check if a hash already exists in the manifest (for deduplication) */
bool mirror_manifest_has_hash(MirrorManifest *m, const char *hash);

/* Write manifest to JSON file
 * Returns true on success, false on I/O or allocation failure */
bool mirror_manifest_write_json(MirrorManifest *m, const char *path);

/* Load manifest from JSON file
 * Returns NULL on failure */
MirrorManifest* mirror_manifest_load_json(const char *path);

/* Set metadata fields */
bool mirror_manifest_set_generated(MirrorManifest *m, const char *timestamp);
bool mirror_manifest_set_source(MirrorManifest *m, const char *source);

#endif /* MIRROR_MANIFEST_H */
