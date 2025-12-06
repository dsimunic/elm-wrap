/**
 * registry_v1.c - Debug commands for V1 protocol registry manipulation
 *
 * Provides commands to inspect and modify the V1 protocol registry.dat file.
 */

#include "debug.h"
#include "../../alloc.h"
#include "../../global_context.h"
#include "../../registry.h"
#include "../../cache.h"
#include "../../log.h"
#include "../package/package_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void print_registry_v1_usage(void) {
    printf("Usage: %s debug registry_v1 SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Manage the V1 protocol registry.dat file.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  list                     Display all packages in the registry\n");
    printf("  add AUTHOR/NAME@VERSION  Add a package version to the registry\n");
    printf("  remove AUTHOR/NAME@VERSION  Remove a package version from the registry\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s debug registry_v1 list\n", global_context_program_name());
    printf("  %s debug registry_v1 add elm/core@1.0.5\n", global_context_program_name());
    printf("  %s debug registry_v1 remove elm/core@1.0.5\n", global_context_program_name());
}

/**
 * List all packages in the registry.
 */
static int cmd_registry_v1_list(void) {
    /* Get cache config to find registry path */
    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        return 1;
    }

    const char *registry_path = cache->registry_path;

    /* Check if registry exists */
    struct stat st;
    if (stat(registry_path, &st) != 0) {
        printf("Registry file does not exist: %s\n", registry_path);
        cache_config_free(cache);
        return 1;
    }

    /* Load registry */
    Registry *registry = registry_load_from_dat(registry_path, NULL);
    if (!registry) {
        log_error("Failed to load registry from: %s", registry_path);
        cache_config_free(cache);
        return 1;
    }

    /* Display registry contents */
    printf("Registry: %s\n", registry_path);
    printf("Total packages: %zu\n", registry->entry_count);
    printf("Total versions: %zu\n", registry->total_versions);
    printf("\n");

    if (registry->entry_count == 0) {
        printf("(empty)\n");
    } else {
        for (size_t i = 0; i < registry->entry_count; i++) {
            RegistryEntry *entry = &registry->entries[i];
            printf("%s/%s\n", entry->author, entry->name);

            for (size_t j = 0; j < entry->version_count; j++) {
                char *ver_str = version_to_string(&entry->versions[j]);
                printf("  - %s\n", ver_str);
                arena_free(ver_str);
            }
        }
    }

    registry_free(registry);
    cache_config_free(cache);
    return 0;
}

/**
 * Add a package version to the registry.
 */
static int cmd_registry_v1_add(const char *package_spec) {
    if (!package_spec) {
        fprintf(stderr, "Error: Package specification required\n");
        fprintf(stderr, "Usage: %s debug registry_v1 add AUTHOR/NAME@VERSION\n", global_context_program_name());
        return 1;
    }

    /* Parse package specification: author/name@version */
    char *spec_copy = arena_strdup(package_spec);
    if (!spec_copy) {
        log_error("Out of memory");
        return 1;
    }

    /* Find @ separator */
    char *at_pos = strchr(spec_copy, '@');
    if (!at_pos) {
        fprintf(stderr, "Error: Invalid package specification '%s'\n", package_spec);
        fprintf(stderr, "Expected format: AUTHOR/NAME@VERSION\n");
        arena_free(spec_copy);
        return 1;
    }

    *at_pos = '\0';
    const char *package_name = spec_copy;
    const char *version_str = at_pos + 1;

    /* Parse author/name */
    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_name, &author, &name)) {
        fprintf(stderr, "Error: Invalid package name '%s'\n", package_name);
        fprintf(stderr, "Expected format: AUTHOR/NAME\n");
        arena_free(spec_copy);
        return 1;
    }

    /* Parse version */
    Version version = version_parse(version_str);
    if (version.major == 0 && version.minor == 0 && version.patch == 0 &&
        strcmp(version_str, "0.0.0") != 0) {
        fprintf(stderr, "Error: Invalid version string '%s'\n", version_str);
        fprintf(stderr, "Expected format: MAJOR.MINOR.PATCH\n");
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    /* Get cache config to find registry path */
    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    const char *registry_path = cache->registry_path;

    /* Load existing registry or create new one */
    Registry *registry = NULL;
    struct stat st;
    if (stat(registry_path, &st) == 0) {
        registry = registry_load_from_dat(registry_path, NULL);
        if (!registry) {
            log_error("Failed to load existing registry from: %s", registry_path);
            cache_config_free(cache);
            arena_free(author);
            arena_free(name);
            arena_free(spec_copy);
            return 1;
        }
    } else {
        registry = registry_create();
        if (!registry) {
            log_error("Failed to create registry");
            cache_config_free(cache);
            arena_free(author);
            arena_free(name);
            arena_free(spec_copy);
            return 1;
        }
    }

    /* Check if version already exists */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (entry) {
        for (size_t i = 0; i < entry->version_count; i++) {
            if (registry_version_compare(&entry->versions[i], &version) == 0) {
                printf("Package %s/%s@%s already exists in registry\n", author, name, version_str);
                registry_free(registry);
                cache_config_free(cache);
                arena_free(author);
                arena_free(name);
                arena_free(spec_copy);
                return 0;
            }
        }
    }

    /* Add version (registry_add_version handles insertion in correct order) */
    if (!registry_add_version(registry, author, name, version)) {
        log_error("Failed to add %s/%s@%s to registry", author, name, version_str);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    /* Write registry back to disk */
    registry_sort_entries(registry);
    if (!registry_dat_write(registry, registry_path)) {
        log_error("Failed to write updated registry to: %s", registry_path);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    printf("Added %s/%s@%s to registry\n", author, name, version_str);

    registry_free(registry);
    cache_config_free(cache);
    arena_free(author);
    arena_free(name);
    arena_free(spec_copy);
    return 0;
}

/**
 * Remove a package version from the registry.
 */
static int cmd_registry_v1_remove(const char *package_spec) {
    if (!package_spec) {
        fprintf(stderr, "Error: Package specification required\n");
        fprintf(stderr, "Usage: %s debug registry_v1 remove AUTHOR/NAME@VERSION\n", global_context_program_name());
        return 1;
    }

    /* Parse package specification: author/name@version */
    char *spec_copy = arena_strdup(package_spec);
    if (!spec_copy) {
        log_error("Out of memory");
        return 1;
    }

    /* Find @ separator */
    char *at_pos = strchr(spec_copy, '@');
    if (!at_pos) {
        fprintf(stderr, "Error: Invalid package specification '%s'\n", package_spec);
        fprintf(stderr, "Expected format: AUTHOR/NAME@VERSION\n");
        arena_free(spec_copy);
        return 1;
    }

    *at_pos = '\0';
    const char *package_name = spec_copy;
    const char *version_str = at_pos + 1;

    /* Parse author/name */
    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_name, &author, &name)) {
        fprintf(stderr, "Error: Invalid package name '%s'\n", package_name);
        fprintf(stderr, "Expected format: AUTHOR/NAME\n");
        arena_free(spec_copy);
        return 1;
    }

    /* Parse version */
    Version version = version_parse(version_str);
    if (version.major == 0 && version.minor == 0 && version.patch == 0 &&
        strcmp(version_str, "0.0.0") != 0) {
        fprintf(stderr, "Error: Invalid version string '%s'\n", version_str);
        fprintf(stderr, "Expected format: MAJOR.MINOR.PATCH\n");
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    /* Get cache config to find registry path */
    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    const char *registry_path = cache->registry_path;

    /* Load existing registry */
    struct stat st;
    if (stat(registry_path, &st) != 0) {
        fprintf(stderr, "Error: Registry file does not exist: %s\n", registry_path);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    Registry *registry = registry_load_from_dat(registry_path, NULL);
    if (!registry) {
        log_error("Failed to load registry from: %s", registry_path);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    /* Find the package entry */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (!entry) {
        fprintf(stderr, "Error: Package %s/%s not found in registry\n", author, name);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    /* Find the version in the entry */
    size_t version_index = (size_t)-1;
    for (size_t i = 0; i < entry->version_count; i++) {
        if (registry_version_compare(&entry->versions[i], &version) == 0) {
            version_index = i;
            break;
        }
    }

    if (version_index == (size_t)-1) {
        fprintf(stderr, "Error: Version %s not found for package %s/%s\n",
                version_str, author, name);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    /* Remove the version */
    if (entry->version_count == 1) {
        /* This is the last version, remove the entire entry */
        arena_free(entry->author);
        arena_free(entry->name);
        arena_free(entry->versions);

        /* Shift entries down */
        size_t entry_index = (size_t)(entry - registry->entries);
        for (size_t i = entry_index; i < registry->entry_count - 1; i++) {
            registry->entries[i] = registry->entries[i + 1];
        }
        registry->entry_count--;
        registry->total_versions--;
    } else {
        /* Remove just this version */
        if (version_index < entry->version_count - 1) {
            memmove(&entry->versions[version_index],
                    &entry->versions[version_index + 1],
                    sizeof(Version) * (entry->version_count - version_index - 1));
        }
        entry->version_count--;
        registry->total_versions--;

        /* Reallocate to shrink array */
        Version *new_versions = arena_realloc(entry->versions,
                                              sizeof(Version) * entry->version_count);
        if (new_versions) {
            entry->versions = new_versions;
        }
    }

    /* Write registry back to disk */
    registry_sort_entries(registry);
    if (!registry_dat_write(registry, registry_path)) {
        log_error("Failed to write updated registry to: %s", registry_path);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        arena_free(spec_copy);
        return 1;
    }

    printf("Removed %s/%s@%s from registry\n", author, name, version_str);

    registry_free(registry);
    cache_config_free(cache);
    arena_free(author);
    arena_free(name);
    arena_free(spec_copy);
    return 0;
}

/**
 * Main entry point for registry_v1 debug command.
 */
int cmd_debug_registry_v1(int argc, char *argv[]) {
    if (argc < 2) {
        print_registry_v1_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_registry_v1_usage();
        return 0;
    }

    if (strcmp(subcmd, "list") == 0) {
        return cmd_registry_v1_list();
    }

    if (strcmp(subcmd, "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Package specification required\n");
            fprintf(stderr, "Usage: %s debug registry_v1 add AUTHOR/NAME@VERSION\n", global_context_program_name());
            return 1;
        }
        return cmd_registry_v1_add(argv[2]);
    }

    if (strcmp(subcmd, "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Package specification required\n");
            fprintf(stderr, "Usage: %s debug registry_v1 remove AUTHOR/NAME@VERSION\n", global_context_program_name());
            return 1;
        }
        return cmd_registry_v1_remove(argv[2]);
    }

    fprintf(stderr, "Error: Unknown registry_v1 subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s debug registry_v1 --help' for usage information.\n", global_context_program_name());
    return 1;
}
