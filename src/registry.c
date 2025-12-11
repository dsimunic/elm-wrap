#include "registry.h"
#include "alloc.h"
#include "constants.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/* Binary I/O helpers */
static bool read_u8(FILE *f, uint8_t *out) {
    return fread(out, 1, 1, f) == 1;
}

static bool read_u64_be(FILE *f, uint64_t *out) {
    uint8_t bytes[8];
    if (fread(bytes, 1, 8, f) != 8) return false;

    *out = ((uint64_t)bytes[0] << 56) |
           ((uint64_t)bytes[1] << 48) |
           ((uint64_t)bytes[2] << 40) |
           ((uint64_t)bytes[3] << 32) |
           ((uint64_t)bytes[4] << 24) |
           ((uint64_t)bytes[5] << 16) |
           ((uint64_t)bytes[6] << 8) |
           ((uint64_t)bytes[7]);

    return true;
}

static bool write_u8(FILE *f, uint8_t val) {
    return fwrite(&val, 1, 1, f) == 1;
}

static bool write_u64_be(FILE *f, uint64_t val) {
    uint8_t bytes[8] = {
        (uint8_t)(val >> 56),
        (uint8_t)(val >> 48),
        (uint8_t)(val >> 40),
        (uint8_t)(val >> 32),
        (uint8_t)(val >> 24),
        (uint8_t)(val >> 16),
        (uint8_t)(val >> 8),
        (uint8_t)(val)
    };

    return fwrite(bytes, 1, 8, f) == 8;
}

/* Version operations */
/* version_parse(), version_to_string(), version_compare() (registry_version_compare),
 * and version_is_constraint() (registry_is_version_constraint) are now implemented
 * in commands/package/package_common.c. See registry.h for compatibility aliases. */

/* Compare two registry entries by author/name alphabetically */
static int registry_entry_compare(const void *a, const void *b) {
    const RegistryEntry *entry_a = (const RegistryEntry *)a;
    const RegistryEntry *entry_b = (const RegistryEntry *)b;

    int author_cmp = strcmp(entry_a->author, entry_b->author);
    if (author_cmp != 0) return author_cmp;

    return strcmp(entry_a->name, entry_b->name);
}

/* Sort registry entries alphabetically by package name */
void registry_sort_entries(Registry *registry) {
    if (!registry || registry->entry_count <= 1) return;

    qsort(registry->entries, registry->entry_count, sizeof(RegistryEntry), registry_entry_compare);
}

/* Registry lifecycle */
Registry* registry_create(void) {
    Registry *registry = arena_calloc(1, sizeof(Registry));
    if (!registry) return NULL;

    registry->capacity = INITIAL_REGISTRY_CAPACITY;
    registry->entries = arena_malloc(sizeof(RegistryEntry) * registry->capacity);

    if (!registry->entries) {
        arena_free(registry);
        return NULL;
    }

    return registry;
}

void registry_free(Registry *registry) {
    if (!registry) return;

    for (size_t i = 0; i < registry->entry_count; i++) {
        arena_free(registry->entries[i].author);
        arena_free(registry->entries[i].name);
        arena_free(registry->entries[i].versions);
    }

    arena_free(registry->entries);
    arena_free(registry);
}

/* Read version from binary file */
static bool read_version(FILE *f, Version *v) {
    uint8_t major;
    if (!read_u8(f, &major)) return false;

    if (major == 255) {
        /* Extended format */
        uint64_t major64, minor64, patch64;
        if (!read_u64_be(f, &major64)) return false;
        if (!read_u64_be(f, &minor64)) return false;
        if (!read_u64_be(f, &patch64)) return false;

        /* Clamp to uint16 for simplicity */
        v->major = (major64 > 65535) ? 65535 : (uint16_t)major64;
        v->minor = (minor64 > 65535) ? 65535 : (uint16_t)minor64;
        v->patch = (patch64 > 65535) ? 65535 : (uint16_t)patch64;
    } else {
        /* Standard format */
        uint8_t minor, patch;
        if (!read_u8(f, &minor)) return false;
        if (!read_u8(f, &patch)) return false;

        v->major = major;
        v->minor = minor;
        v->patch = patch;
    }

    return true;
}

/* Write version to binary file */
static bool write_version(FILE *f, const Version *v) {
    if (v->major < 255 && v->minor < 256 && v->patch < 256) {
        /* Standard format */
        if (!write_u8(f, (uint8_t)v->major)) return false;
        if (!write_u8(f, (uint8_t)v->minor)) return false;
        if (!write_u8(f, (uint8_t)v->patch)) return false;
    } else {
        /* Extended format */
        if (!write_u8(f, 255)) return false;
        if (!write_u64_be(f, v->major)) return false;
        if (!write_u64_be(f, v->minor)) return false;
        if (!write_u64_be(f, v->patch)) return false;
    }

    return true;
}

/* Load registry from binary file */
Registry* registry_load_from_dat(const char *path, size_t *known_count) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    Registry *registry = registry_create();
    if (!registry) {
        fclose(f);
        return NULL;
    }

    /* Read header: total version count */
    uint64_t total_versions;
    if (!read_u64_be(f, &total_versions)) {
        log_error("Failed to read registry header from %s", path);
        registry_free(registry);
        fclose(f);
        return NULL;
    }

    registry->total_versions = (size_t)total_versions;
    if (known_count) {
        *known_count = registry->total_versions;
    }

    /* Read entry count */
    uint64_t entry_count;
    if (!read_u64_be(f, &entry_count)) {
        log_error("Failed to read registry entry count from %s", path);
        registry_free(registry);
        fclose(f);
        return NULL;
    }

    /* Ensure capacity */
    if (entry_count > registry->capacity) {
        registry->capacity = entry_count;
        RegistryEntry *new_entries = arena_realloc(registry->entries, sizeof(RegistryEntry) * registry->capacity);
        if (!new_entries) {
            registry_free(registry);
            fclose(f);
            return NULL;
        }
        registry->entries = new_entries;
    }

    /* Read entries */
    for (uint64_t i = 0; i < entry_count; i++) {
        /* Read author */
        uint8_t author_len;
        if (!read_u8(f, &author_len)) {
            log_error("Failed to read author length at entry %llu", (unsigned long long)i);
            registry_free(registry);
            fclose(f);
            return NULL;
        }

        char *author = arena_malloc(author_len + 1);
        if (!author || fread(author, 1, author_len, f) != author_len) {
            arena_free(author);
            registry_free(registry);
            fclose(f);
            return NULL;
        }
        author[author_len] = '\0';

        /* Read project name */
        uint8_t name_len;
        if (!read_u8(f, &name_len)) {
            log_error("Failed to read name length at entry %llu", (unsigned long long)i);
            arena_free(author);
            registry_free(registry);
            fclose(f);
            return NULL;
        }

        char *name = arena_malloc(name_len + 1);
        if (!name || fread(name, 1, name_len, f) != name_len) {
            arena_free(author);
            arena_free(name);
            registry_free(registry);
            fclose(f);
            return NULL;
        }
        name[name_len] = '\0';

        /* Read newest version */
        Version newest;
        if (!read_version(f, &newest)) {
            log_error("Failed to read newest version for %s/%s", author, name);
            arena_free(author);
            arena_free(name);
            registry_free(registry);
            fclose(f);
            return NULL;
        }

        /* Read previous versions count */
        uint64_t prev_count;
        if (!read_u64_be(f, &prev_count)) {
            log_error("Failed to read previous version count for %s/%s", author, name);
            arena_free(author);
            arena_free(name);
            registry_free(registry);
            fclose(f);
            return NULL;
        }

        /* Allocate versions array */
        size_t total_version_count = 1 + (size_t)prev_count;
        Version *versions = arena_malloc(sizeof(Version) * total_version_count);
        if (!versions) {
            arena_free(author);
            arena_free(name);
            registry_free(registry);
            fclose(f);
            return NULL;
        }

        versions[0] = newest;

        /* Read previous versions */
        for (uint64_t j = 0; j < prev_count; j++) {
            if (!read_version(f, &versions[1 + j])) {
                log_error("Failed to read version %llu for %s/%s", (unsigned long long)j, author, name);
                arena_free(author);
                arena_free(name);
                arena_free(versions);
                registry_free(registry);
                fclose(f);
                return NULL;
            }
        }

        /* Add to registry */
        registry->entries[registry->entry_count].author = author;
        registry->entries[registry->entry_count].name = name;
        registry->entries[registry->entry_count].versions = versions;
        registry->entries[registry->entry_count].version_count = total_version_count;
        registry->entry_count++;
    }

    /* Ensure entries are sorted */
    registry_sort_entries(registry);

    fclose(f);
    return registry;
}

/* Write registry to binary file */
bool registry_dat_write(const Registry *registry, const char *path) {
    if (!registry || !path) return false;

    /* Write to temporary file first */
    char tmp_path[MAX_TEMP_PATH_LENGTH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        log_error("Failed to open %s for writing: %s", tmp_path, strerror(errno));
        return false;
    }

    /* Write header: total version count */
    if (!write_u64_be(f, registry->total_versions)) {
        log_error("Failed to write registry header");
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    /* Write entry count */
    if (!write_u64_be(f, registry->entry_count)) {
        log_error("Failed to write entry count");
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    /* Write entries */
    for (size_t i = 0; i < registry->entry_count; i++) {
        RegistryEntry *entry = &registry->entries[i];

        /* Write author */
        uint8_t author_len = (uint8_t)strlen(entry->author);
        if (!write_u8(f, author_len) || fwrite(entry->author, 1, author_len, f) != author_len) {
            log_error("Failed to write author for entry %zu", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write name */
        uint8_t name_len = (uint8_t)strlen(entry->name);
        if (!write_u8(f, name_len) || fwrite(entry->name, 1, name_len, f) != name_len) {
            log_error("Failed to write name for entry %zu", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write newest version (first in array) */
        if (entry->version_count == 0) {
            log_error("Entry %zu has no versions", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        if (!write_version(f, &entry->versions[0])) {
            log_error("Failed to write newest version for entry %zu", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write previous versions count */
        uint64_t prev_count = entry->version_count - 1;
        if (!write_u64_be(f, prev_count)) {
            log_error("Failed to write previous version count for entry %zu", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write previous versions */
        for (size_t j = 1; j < entry->version_count; j++) {
            if (!write_version(f, &entry->versions[j])) {
                log_error("Failed to write version %zu for entry %zu", j, i);
                fclose(f);
                unlink(tmp_path);
                return false;
            }
        }
    }

    /* Flush and sync */
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        log_error("Failed to rename %s to %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return false;
    }

    return true;
}

/* Registry lookup */
RegistryEntry* registry_find(Registry *registry, const char *author, const char *name) {
    if (!registry || !author || !name) return NULL;

    for (size_t i = 0; i < registry->entry_count; i++) {
        if (strcmp(registry->entries[i].author, author) == 0 &&
            strcmp(registry->entries[i].name, name) == 0) {
            return &registry->entries[i];
        }
    }

    return NULL;
}

bool registry_contains(Registry *registry, const char *author, const char *name) {
    return registry_find(registry, author, name) != NULL;
}

/* Registry modification */
bool registry_add_entry(Registry *registry, const char *author, const char *name) {
    if (!registry || !author || !name) return false;

    /* Check if entry already exists */
    if (registry_find(registry, author, name)) {
        return true;  /* Already exists */
    }

    /* Expand capacity if needed */
    if (registry->entry_count >= registry->capacity) {
        registry->capacity *= 2;
        RegistryEntry *new_entries = arena_realloc(registry->entries, sizeof(RegistryEntry) * registry->capacity);
        if (!new_entries) return false;
        registry->entries = new_entries;
    }

    /* Add new entry */
    RegistryEntry *entry = &registry->entries[registry->entry_count];
    entry->author = arena_strdup(author);
    entry->name = arena_strdup(name);
    entry->versions = NULL;
    entry->version_count = 0;

    if (!entry->author || !entry->name) {
        arena_free(entry->author);
        arena_free(entry->name);
        return false;
    }

    registry->entry_count++;
    return true;
}

bool registry_add_version(Registry *registry, const char *author, const char *name, Version version) {
    if (!registry || !author || !name) return false;

    /* Find or create entry */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (!entry) {
        if (!registry_add_entry(registry, author, name)) {
            return false;
        }
        entry = registry_find(registry, author, name);
        if (!entry) return false;
    }

    /* Check if version already exists */
    for (size_t i = 0; i < entry->version_count; i++) {
        if (registry_version_compare(&entry->versions[i], &version) == 0) {
            return true;  /* Already exists */
        }
    }

    /* Add version */
    Version *new_versions = arena_realloc(entry->versions, sizeof(Version) * (entry->version_count + 1));
    if (!new_versions) return false;

    entry->versions = new_versions;

    /* Insert in descending order (newest first) */
    size_t insert_pos = 0;
    for (size_t i = 0; i < entry->version_count; i++) {
        if (registry_version_compare(&version, &entry->versions[i]) > 0) {
            insert_pos = i;
            break;
        }
        insert_pos = i + 1;
    }

    /* Shift versions if needed */
    if (insert_pos < entry->version_count) {
        memmove(&entry->versions[insert_pos + 1], &entry->versions[insert_pos],
                sizeof(Version) * (entry->version_count - insert_pos));
    }

    entry->versions[insert_pos] = version;
    entry->version_count++;
    registry->total_versions++;

    return true;
}

/* Parse Elm version constraint "1.0.0 <= v < 2.0.0" and find highest matching version */
bool registry_resolve_constraint(Registry *registry, const char *author, const char *name,
                                 const char *constraint, Version *out_version) {
    if (!registry || !author || !name || !constraint || !out_version) {
        return false;
    }

    /* Parse constraint using unified version_parse_constraint */
    VersionRange range;
    if (!version_parse_constraint(constraint, &range)) {
        return false;
    }

    /* Find the package in the registry */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (!entry || entry->version_count == 0) {
        return false;
    }

    /* Versions are stored newest first, so iterate and find the first (highest) matching one */
    for (size_t i = 0; i < entry->version_count; i++) {
        Version *v = &entry->versions[i];

        /* Check if version is within range */
        if (version_in_range(v, &range)) {
            *out_version = *v;
            return true;
        }
    }

    return false;  /* No matching version found */
}

/* Utility */
void registry_print(const Registry *registry) {
    if (!registry) return;

    printf("Registry: %zu packages, %zu total versions\n", registry->entry_count, registry->total_versions);

    for (size_t i = 0; i < registry->entry_count && i < 10; i++) {
        RegistryEntry *entry = &registry->entries[i];
        printf("  %s/%s: %zu versions\n", entry->author, entry->name, entry->version_count);

        for (size_t j = 0; j < entry->version_count && j < 5; j++) {
            char *ver_str = version_to_string(&entry->versions[j]);
            printf("    - %s\n", ver_str);
            arena_free(ver_str);
        }

        if (entry->version_count > 5) {
            printf("    ... and %zu more\n", entry->version_count - 5);
        }
    }

    if (registry->entry_count > 10) {
        printf("  ... and %zu more packages\n", registry->entry_count - 10);
    }
}

/* Merge local-dev registry into main registry */
bool registry_merge_local_dev(Registry *registry, const char *local_dev_path) {
    if (!registry || !local_dev_path) {
        return false;
    }
    
    /* Try to load local-dev registry */
    Registry *local_dev = registry_load_from_dat(local_dev_path, NULL);
    if (!local_dev) {
        /* File doesn't exist or couldn't be read - that's fine */
        return true;
    }
    
    /* Merge each entry */
    for (size_t i = 0; i < local_dev->entry_count; i++) {
        RegistryEntry *local_entry = &local_dev->entries[i];
        
        /* Find or create entry in main registry */
        RegistryEntry *main_entry = registry_find(registry, local_entry->author, local_entry->name);
        
        if (!main_entry) {
            /* Add new entry */
            if (!registry_add_entry(registry, local_entry->author, local_entry->name)) {
                registry_free(local_dev);
                return false;
            }
            main_entry = registry_find(registry, local_entry->author, local_entry->name);
        }
        
        if (!main_entry) {
            registry_free(local_dev);
            return false;
        }
        
        /* Add versions that don't exist */
        for (size_t j = 0; j < local_entry->version_count; j++) {
            Version *v = &local_entry->versions[j];
            
            /* Check if version exists */
            bool exists = false;
            for (size_t k = 0; k < main_entry->version_count; k++) {
                if (main_entry->versions[k].major == v->major &&
                    main_entry->versions[k].minor == v->minor &&
                    main_entry->versions[k].patch == v->patch) {
                    exists = true;
                    break;
                }
            }
            
            if (!exists) {
                registry_add_version(registry, local_entry->author, local_entry->name, *v);
            }
        }
    }
    
    registry_sort_entries(registry);
    registry_free(local_dev);
    return true;
}

