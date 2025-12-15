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
#include "../../install_env.h"
#include "../../log.h"
#include "../../fileutil.h"
#include "../../constants.h"
#include "../../vendor/cJSON.h"
#include "../package/package_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static void print_registry_v1_usage(void) {
    printf("Usage: %s debug registry_v1 SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Manage the V1 protocol registry.dat file.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  list                     Display all packages in the registry\n");
    printf("  add AUTHOR/NAME@VERSION  Add a package version to the registry\n");
    printf("  remove AUTHOR/NAME@VERSION  Remove a package version from the registry\n");
    printf("  apply-since JSON_PATH    Apply a /since JSON response offline\n");
    printf("  reset [--yes|-y]         Delete registry.dat and re-download it\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help message\n");
    printf("  -y, --yes               Assume yes for prompts\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s debug registry_v1 list\n", global_context_program_name());
    printf("  %s debug registry_v1 add elm/core@1.0.5\n", global_context_program_name());
    printf("  %s debug registry_v1 remove elm/core@1.0.5\n", global_context_program_name());
    printf("  %s debug registry_v1 apply-since /path/to/since.json\n", global_context_program_name());
    printf("  %s debug registry_v1 reset\n", global_context_program_name());
}

/**
 * List all packages in the registry.
 */
static int cmd_registry_v1_list(int argc, char *argv[]) {
    /* Check for help flag */
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: %s debug registry_v1 list\n", global_context_program_name());
        printf("\n");
        printf("Display all packages in the registry.\n");
        printf("\n");
        printf("This command lists all packages and their versions currently\n");
        printf("registered in the V1 protocol registry.dat file.\n");
        return 0;
    }

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
    printf("Since count: %zu\n", registry->since_count);
    printf("Versions in map: %zu\n", registry_versions_in_map_count(registry));
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

static bool prompt_offline_reset_proceed(void) {
    fprintf(stderr,
            "You are in offline mode and will not be able to download a fresh registry index after the reset. Proceed [Y/n] ");

    char response[MAX_TEMP_BUFFER_LENGTH];
    if (!fgets(response, sizeof(response), stdin)) {
        return false;
    }

    for (size_t i = 0; response[i] != '\0'; i++) {
        char c = response[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c == 'n' || c == 'N') {
            return false;
        }
        return true;
    }

    /* Default to yes on empty response */
    return true;
}

static bool has_yes_flag(int argc, char *argv[]) {
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0) {
            return true;
        }
    }
    return false;
}

static bool has_help_flag(int argc, char *argv[]) {
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            return true;
        }
    }
    return false;
}

static bool delete_regular_file_if_exists(const char *path) {
    if (!path || path[0] == '\0') return true;
    if (!file_exists(path)) return true;
    if (unlink(path) != 0) {
        log_error("Failed to delete %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static int cmd_registry_v1_reset(int argc, char *argv[]) {
    /* Check for help flag */
    if (has_help_flag(argc, argv)) {
        printf("Usage: %s debug registry_v1 reset [--yes|-y]\n", global_context_program_name());
        printf("\n");
        printf("Delete registry.dat and re-download it.\n");
        printf("\n");
        printf("This command removes the local V1 protocol registry.dat file\n");
        printf("and its associated metadata (ETag, since count), then downloads\n");
        printf("a fresh copy from the registry server. This is useful when the\n");
        printf("local registry cache becomes corrupted or out of sync.\n");
        printf("\n");
        printf("Options:\n");
        printf("  -y, --yes    Assume yes for prompts (skip confirmation)\n");
        printf("  -h, --help   Show this help message\n");
        return 0;
    }

    bool assume_yes = has_yes_flag(argc, argv);

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to allocate install environment");
        return 1;
    }

    if (!install_env_prepare_v1(env)) {
        log_error("Failed to initialize V1 registry environment");
        install_env_free(env);
        return 1;
    }

    if (env->offline) {
        if (!assume_yes && !prompt_offline_reset_proceed()) {
            log_progress("Aborted");
            install_env_free(env);
            return 0;
        }
    }

    const char *registry_path = env->cache ? env->cache->registry_path : NULL;
    if (!registry_path) {
        log_error("Registry path is not available");
        install_env_free(env);
        return 1;
    }

    char *etag_path = install_env_registry_etag_file_path(registry_path);
    char *since_path = install_env_registry_since_count_file_path(registry_path);

    bool deleted_ok = delete_regular_file_if_exists(registry_path) &&
                      delete_regular_file_if_exists(etag_path) &&
                      delete_regular_file_if_exists(since_path);

    if (etag_path) arena_free(etag_path);
    if (since_path) arena_free(since_path);

    if (!deleted_ok) {
        install_env_free(env);
        return 1;
    }

    log_progress("Deleted registry cache: %s", registry_path);

    if (env->offline) {
        log_warn("Offline mode: cannot download a fresh registry index until you are online");
        install_env_free(env);
        return 0;
    }

    if (global_context_skip_registry_update()) {
        log_progress("Skipping registry download (WRAP_SKIP_REGISTRY_UPDATE=1)");
        log_progress("Registry reset complete");
        install_env_free(env);
        return 0;
    }

    if (!install_env_ensure_v1_registry(env)) {
        log_error("Failed to download a fresh registry index after reset");
        install_env_free(env);
        return 1;
    }

    log_progress("Registry reset complete");
    install_env_free(env);
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
    char *author = NULL;
    char *name = NULL;
    Version version;
    if (!parse_package_with_version(package_spec, &author, &name, &version)) {
        fprintf(stderr, "Error: Invalid package specification '%s'\n", package_spec);
        fprintf(stderr, "Expected format: AUTHOR/NAME@VERSION\n");
        return 1;
    }

    /* Get cache config to find registry path */
    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        arena_free(author);
        arena_free(name);
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
            return 1;
        }
    } else {
        registry = registry_create();
        if (!registry) {
            log_error("Failed to create registry");
            cache_config_free(cache);
            arena_free(author);
            arena_free(name);
            return 1;
        }
    }

    /* Check if version already exists */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (entry) {
        for (size_t i = 0; i < entry->version_count; i++) {
            if (registry_version_compare(&entry->versions[i], &version) == 0) {
                char *ver_str = version_to_string(&version);
                printf("Package %s/%s@%s already exists in registry\n", author, name, ver_str ? ver_str : "(unknown)");
                if (ver_str) arena_free(ver_str);
                registry_free(registry);
                cache_config_free(cache);
                arena_free(author);
                arena_free(name);
                return 0;
            }
        }
    }

    /* Add version (registry_add_version handles insertion in correct order) */
    if (!registry_add_version(registry, author, name, version)) {
        char *ver_str = version_to_string(&version);
        log_error("Failed to add %s/%s@%s to registry", author, name, ver_str ? ver_str : "(unknown)");
        if (ver_str) arena_free(ver_str);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
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
        return 1;
    }

    char *ver_str = version_to_string(&version);
    printf("Added %s/%s@%s to registry\n", author, name, ver_str ? ver_str : "(unknown)");
    if (ver_str) arena_free(ver_str);

    registry_free(registry);
    cache_config_free(cache);
    arena_free(author);
    arena_free(name);
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
    char *author = NULL;
    char *name = NULL;
    Version version;
    if (!parse_package_with_version(package_spec, &author, &name, &version)) {
        fprintf(stderr, "Error: Invalid package specification '%s'\n", package_spec);
        fprintf(stderr, "Expected format: AUTHOR/NAME@VERSION\n");
        return 1;
    }

    /* Get cache config to find registry path */
    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        arena_free(author);
        arena_free(name);
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
        return 1;
    }

    Registry *registry = registry_load_from_dat(registry_path, NULL);
    if (!registry) {
        log_error("Failed to load registry from: %s", registry_path);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
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
        char *ver_str = version_to_string(&version);
        fprintf(stderr, "Error: Version %s not found for package %s/%s\n",
                ver_str ? ver_str : "(unknown)", author, name);
        if (ver_str) arena_free(ver_str);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    /* Remove the version */
    bool removed = false;
    if (!registry_remove_version_ex(registry, author, name, version, true, &removed) || !removed) {
        char *ver_str = version_to_string(&version);
        fprintf(stderr, "Error: Failed to remove %s/%s@%s from registry\n",
                author, name, ver_str ? ver_str : "(unknown)");
        if (ver_str) arena_free(ver_str);
        registry_free(registry);
        cache_config_free(cache);
        arena_free(author);
        arena_free(name);
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
        return 1;
    }

    char *ver_str = version_to_string(&version);
    printf("Removed %s/%s@%s from registry\n", author, name, ver_str ? ver_str : "(unknown)");
    if (ver_str) arena_free(ver_str);

    registry_free(registry);
    cache_config_free(cache);
    arena_free(author);
    arena_free(name);
    return 0;
}

/**
 * Apply an offline /since JSON response (array of "author/name@version" strings).
 *
 * This is primarily for testing and debugging the since_count advancement rules.
 */
static int cmd_registry_v1_apply_since(const char *json_path) {
    if (!json_path || json_path[0] == '\0') {
        fprintf(stderr, "Error: JSON file path required\n");
        fprintf(stderr, "Usage: %s debug registry_v1 apply-since JSON_PATH\n", global_context_program_name());
        return 1;
    }

    char *json_str = file_read_contents(json_path);
    if (!json_str) {
        fprintf(stderr, "Error: Failed to read JSON file: %s\n", json_path);
        return 1;
    }

    cJSON *json = cJSON_Parse(json_str);
    if (!json || !cJSON_IsArray(json)) {
        fprintf(stderr, "Error: Failed to parse /since JSON array in %s\n", json_path);
        if (json) cJSON_Delete(json);
        arena_free(json_str);
        return 1;
    }

    int count = cJSON_GetArraySize(json);
    if (count < 0) count = 0;

    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        cJSON_Delete(json);
        arena_free(json_str);
        return 1;
    }

    const char *registry_path = cache->registry_path;

    Registry *registry = NULL;
    struct stat st;
    if (stat(registry_path, &st) == 0) {
        registry = registry_load_from_dat(registry_path, NULL);
        if (!registry) {
            log_error("Failed to load existing registry from: %s", registry_path);
            cache_config_free(cache);
            cJSON_Delete(json);
            arena_free(json_str);
            return 1;
        }
    } else {
        registry = registry_create();
        if (!registry) {
            log_error("Failed to create registry");
            cache_config_free(cache);
            cJSON_Delete(json);
            arena_free(json_str);
            return 1;
        }
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, json) {
        if (!cJSON_IsString(item)) {
            continue;
        }

        const char *entry_str = item->valuestring;
        if (!entry_str) {
            continue;
        }

        char *author = NULL;
        char *name = NULL;
        Version version;
        if (!parse_package_with_version(entry_str, &author, &name, &version)) {
            fprintf(stderr, "Error: Invalid /since entry: %s\n", entry_str);
            registry_free(registry);
            cache_config_free(cache);
            cJSON_Delete(json);
            arena_free(json_str);
            return 1;
        }

        if (!registry_add_version_ex(registry, author, name, version, false, NULL)) {
            log_error("Failed to apply /since entry: %s", entry_str);
            arena_free(author);
            arena_free(name);
            registry_free(registry);
            cache_config_free(cache);
            cJSON_Delete(json);
            arena_free(json_str);
            return 1;
        }

        arena_free(author);
        arena_free(name);
    }

    if (count > 0) {
        size_t add = (size_t)count;
        if (registry->since_count > SIZE_MAX - add) {
            log_error("since_count overflow while applying /since response");
            registry_free(registry);
            cache_config_free(cache);
            cJSON_Delete(json);
            arena_free(json_str);
            return 1;
        }
        registry->since_count += add;
    }

    registry_sort_entries(registry);
    if (!registry_dat_write(registry, registry_path)) {
        log_error("Failed to write updated registry to: %s", registry_path);
        registry_free(registry);
        cache_config_free(cache);
        cJSON_Delete(json);
        arena_free(json_str);
        return 1;
    }

    printf("Applied /since response (%d item%s). since_count is now %zu\n",
           count, count == 1 ? "" : "s", registry->since_count);

    registry_free(registry);
    cache_config_free(cache);
    cJSON_Delete(json);
    arena_free(json_str);
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
        return cmd_registry_v1_list(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "add") == 0) {
        /* Check for help flag */
        if (argc >= 3 && (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)) {
            printf("Usage: %s debug registry_v1 add AUTHOR/NAME@VERSION\n", global_context_program_name());
            printf("\n");
            printf("Add a package version to the registry.\n");
            printf("\n");
            printf("This command adds a new package version to the V1 protocol\n");
            printf("registry.dat file. The package must be specified in the\n");
            printf("format AUTHOR/NAME@VERSION (e.g., elm/core@1.0.5).\n");
            return 0;
        }
        if (argc < 3) {
            fprintf(stderr, "Error: Package specification required\n");
            fprintf(stderr, "Usage: %s debug registry_v1 add AUTHOR/NAME@VERSION\n", global_context_program_name());
            return 1;
        }
        return cmd_registry_v1_add(argv[2]);
    }

    if (strcmp(subcmd, "remove") == 0) {
        /* Check for help flag */
        if (argc >= 3 && (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)) {
            printf("Usage: %s debug registry_v1 remove AUTHOR/NAME@VERSION\n", global_context_program_name());
            printf("\n");
            printf("Remove a package version from the registry.\n");
            printf("\n");
            printf("This command removes a package version from the V1 protocol\n");
            printf("registry.dat file. The package must be specified in the\n");
            printf("format AUTHOR/NAME@VERSION (e.g., elm/core@1.0.5).\n");
            return 0;
        }
        if (argc < 3) {
            fprintf(stderr, "Error: Package specification required\n");
            fprintf(stderr, "Usage: %s debug registry_v1 remove AUTHOR/NAME@VERSION\n", global_context_program_name());
            return 1;
        }
        return cmd_registry_v1_remove(argv[2]);
    }

    if (strcmp(subcmd, "apply-since") == 0) {
        /* Check for help flag */
        if (argc >= 3 && (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)) {
            printf("Usage: %s debug registry_v1 apply-since JSON_PATH\n", global_context_program_name());
            printf("\n");
            printf("Apply a /since JSON response offline.\n");
            printf("\n");
            printf("This command processes a registry /since endpoint JSON response\n");
            printf("that has been saved to a file, and applies the updates to the\n");
            printf("local registry.dat file. This is useful for testing or debugging\n");
            printf("registry synchronization without making network requests.\n");
            return 0;
        }
        if (argc < 3) {
            fprintf(stderr, "Error: JSON file path required\n");
            fprintf(stderr, "Usage: %s debug registry_v1 apply-since JSON_PATH\n", global_context_program_name());
            return 1;
        }
        return cmd_registry_v1_apply_since(argv[2]);
    }

    if (strcmp(subcmd, "reset") == 0) {
        return cmd_registry_v1_reset(argc - 2, argv + 2);
    }

    fprintf(stderr, "Error: Unknown registry_v1 subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s debug registry_v1 --help' for usage information.\n", global_context_program_name());
    return 1;
}
