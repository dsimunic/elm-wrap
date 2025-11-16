#include "registry.h"
#include "alloc.h"
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
Version version_parse(const char *version_str) {
    Version v = {0, 0, 0};

    if (!version_str) return v;

    /* Parse major.minor.patch */
    int major, minor, patch;
    if (sscanf(version_str, "%d.%d.%d", &major, &minor, &patch) == 3) {
        v.major = (uint16_t)major;
        v.minor = (uint16_t)minor;
        v.patch = (uint16_t)patch;
    }

    return v;
}

int registry_version_compare(const Version *a, const Version *b) {
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    return a->patch - b->patch;
}

char* version_to_string(const Version *v) {
    if (!v) return NULL;

    char *str = arena_malloc(32);  /* Enough for "65535.65535.65535" */
    if (!str) return NULL;

    snprintf(str, 32, "%u.%u.%u", v->major, v->minor, v->patch);
    return str;
}

/* Registry lifecycle */
Registry* registry_create(void) {
    Registry *registry = arena_calloc(1, sizeof(Registry));
    if (!registry) return NULL;

    registry->capacity = 128;
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
        fprintf(stderr, "Error: Failed to read registry header from %s\n", path);
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
        fprintf(stderr, "Error: Failed to read registry entry count from %s\n", path);
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
            fprintf(stderr, "Error: Failed to read author length at entry %llu\n", (unsigned long long)i);
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
            fprintf(stderr, "Error: Failed to read name length at entry %llu\n", (unsigned long long)i);
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
            fprintf(stderr, "Error: Failed to read newest version for %s/%s\n", author, name);
            arena_free(author);
            arena_free(name);
            registry_free(registry);
            fclose(f);
            return NULL;
        }

        /* Read previous versions count */
        uint64_t prev_count;
        if (!read_u64_be(f, &prev_count)) {
            fprintf(stderr, "Error: Failed to read previous version count for %s/%s\n", author, name);
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
                fprintf(stderr, "Error: Failed to read version %llu for %s/%s\n", (unsigned long long)j, author, name);
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

    fclose(f);
    return registry;
}

/* Write registry to binary file */
bool registry_dat_write(const Registry *registry, const char *path) {
    if (!registry || !path) return false;

    /* Write to temporary file first */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        fprintf(stderr, "Error: Failed to open %s for writing: %s\n", tmp_path, strerror(errno));
        return false;
    }

    /* Write header: total version count */
    if (!write_u64_be(f, registry->total_versions)) {
        fprintf(stderr, "Error: Failed to write registry header\n");
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    /* Write entry count */
    if (!write_u64_be(f, registry->entry_count)) {
        fprintf(stderr, "Error: Failed to write entry count\n");
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
            fprintf(stderr, "Error: Failed to write author for entry %zu\n", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write name */
        uint8_t name_len = (uint8_t)strlen(entry->name);
        if (!write_u8(f, name_len) || fwrite(entry->name, 1, name_len, f) != name_len) {
            fprintf(stderr, "Error: Failed to write name for entry %zu\n", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write newest version (first in array) */
        if (entry->version_count == 0) {
            fprintf(stderr, "Error: Entry %zu has no versions\n", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        if (!write_version(f, &entry->versions[0])) {
            fprintf(stderr, "Error: Failed to write newest version for entry %zu\n", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write previous versions count */
        uint64_t prev_count = entry->version_count - 1;
        if (!write_u64_be(f, prev_count)) {
            fprintf(stderr, "Error: Failed to write previous version count for entry %zu\n", i);
            fclose(f);
            unlink(tmp_path);
            return false;
        }

        /* Write previous versions */
        for (size_t j = 1; j < entry->version_count; j++) {
            if (!write_version(f, &entry->versions[j])) {
                fprintf(stderr, "Error: Failed to write version %zu for entry %zu\n", j, i);
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
        fprintf(stderr, "Error: Failed to rename %s to %s: %s\n", tmp_path, path, strerror(errno));
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

