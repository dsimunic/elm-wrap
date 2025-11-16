#ifndef REGISTRY_H
#define REGISTRY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Version representation */
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} Version;

/* Package registry entry */
typedef struct {
    char *author;
    char *name;
    Version *versions;      /* Array of versions (newest first) */
    size_t version_count;
} RegistryEntry;

/* Package registry */
typedef struct {
    RegistryEntry *entries;
    size_t entry_count;
    size_t capacity;
    size_t total_versions;  /* Total count across all packages */
} Registry;

/* Registry lifecycle */
Registry* registry_create(void);
void registry_free(Registry *registry);

/* Registry I/O */
Registry* registry_load_from_dat(const char *path, size_t *known_count);
bool registry_dat_write(const Registry *registry, const char *path);

/* Registry lookup */
RegistryEntry* registry_find(Registry *registry, const char *author, const char *name);
bool registry_contains(Registry *registry, const char *author, const char *name);

/* Registry modification */
bool registry_add_entry(Registry *registry, const char *author, const char *name);
bool registry_add_version(Registry *registry, const char *author, const char *name, Version version);

/* Version operations */
Version version_parse(const char *version_str);
int registry_version_compare(const Version *a, const Version *b);
char* version_to_string(const Version *v);

/* Utility */
void registry_print(const Registry *registry);

#endif /* REGISTRY_H */
