#include "package_common.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../cache.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../http_client.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../fileutil.h"
#include "../../commands/cache/check/cache_check.h"
#include "../../commands/cache/full_scan/cache_full_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

// Track packages downloaded during cache operation
typedef struct {
    char **packages;      // Array of "author/name@version" strings
    size_t count;
    size_t capacity;
} CacheDownloadList;

static CacheDownloadList* cache_download_list_create(void) {
    CacheDownloadList *list = arena_malloc(sizeof(CacheDownloadList));
    if (!list) return NULL;
    list->capacity = 16;
    list->count = 0;
    list->packages = arena_malloc(sizeof(char *) * list->capacity);
    if (!list->packages) {
        arena_free(list);
        return NULL;
    }
    return list;
}

static void cache_download_list_add(CacheDownloadList *list, const char *author, const char *name, const char *version) {
    if (!list) return;

    for (size_t i = 0; i < list->count; i++) {
        if (list->packages[i]) {
            char check[512];
            snprintf(check, sizeof(check), "%s/%s@%s", author, name, version);
            if (strcmp(list->packages[i], check) == 0) {
                return;
            }
        }
    }

    if (list->count >= list->capacity) {
        list->capacity *= 2;
        char **new_packages = arena_realloc(list->packages, sizeof(char *) * list->capacity);
        if (!new_packages) return;
        list->packages = new_packages;
    }

    char *entry = arena_malloc(MAX_TEMP_BUFFER_LENGTH);
    if (entry) {
        snprintf(entry, MAX_TEMP_BUFFER_LENGTH, "%s/%s@%s", author, name, version);
        list->packages[list->count++] = entry;
    }
}

static void cache_download_list_free(CacheDownloadList *list) {
    if (!list) return;
    if (list->packages) {
        for (size_t i = 0; i < list->count; i++) {
            if (list->packages[i]) {
                arena_free(list->packages[i]);
            }
        }
        arena_free(list->packages);
    }
    arena_free(list);
}

static bool cache_download_package_recursive(InstallEnv *env, const char *author, const char *name, const char *version, CacheDownloadList *downloaded) {
    if (!env || !author || !name || !version) return false;

    if (cache_package_fully_downloaded(env->cache, author, name, version)) {
        log_debug("Package %s/%s %s already cached (verified src/ exists)", author, name, version);
        return true;
    }

    if (cache_package_exists(env->cache, author, name, version)) {
        log_debug("Package %s/%s %s directory exists but src/ is missing - re-downloading", author, name, version);
    }

    log_progress("Downloading %s/%s %s...", author, name, version);
    if (!install_env_download_package(env, author, name, version)) {
        fprintf(stderr, "Error: Failed to download %s/%s %s\n", author, name, version);
        return false;
    }

    cache_download_list_add(downloaded, author, name, version);

    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        fprintf(stderr, "Error: Failed to get package path for %s/%s %s\n", author, name, version);
        return false;
    }

    char elm_json_path[2048];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", pkg_path);
    arena_free(pkg_path);

    ElmJson *pkg_elm_json = elm_json_read(elm_json_path);
    if (!pkg_elm_json) {
        log_debug("Could not read elm.json for %s/%s %s, skipping dependencies", author, name, version);
        return true;
    }

    bool success = true;
    if (pkg_elm_json->type == ELM_PROJECT_PACKAGE && pkg_elm_json->package_dependencies) {
        for (int i = 0; i < pkg_elm_json->package_dependencies->count; i++) {
            Package *dep = &pkg_elm_json->package_dependencies->packages[i];
            if (dep->author && dep->name) {
                char *dep_version = NULL;
                if (dep->version && registry_is_version_constraint(dep->version)) {
                    Version resolved;
                    if (registry_resolve_constraint(env->registry, dep->author, dep->name, dep->version, &resolved)) {
                        dep_version = version_to_string(&resolved);
                    }
                } else {
                    dep_version = arena_strdup(dep->version);
                }

                if (dep_version) {
                    if (!cache_download_package_recursive(env, dep->author, dep->name, dep_version, downloaded)) {
                        success = false;
                    }
                    arena_free(dep_version);
                }
            }
        }
    }

    elm_json_free(pkg_elm_json);
    return success;
}

static void print_cache_usage(void) {
    printf("Usage: %s package cache SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Cache management commands.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  <PACKAGE> [VERSION]                Download package to cache\n");
    printf("  check <PACKAGE>                    Check cache status for a package\n");
    printf("  full-scan                          Scan entire cache and verify all packages\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package cache elm/html                  # Download latest elm/html\n", global_context_program_name());
    printf("  %s package cache elm/html 1.0.0            # Download specific version\n", global_context_program_name());
    printf("  %s package cache check elm/html            # Check cache status for elm/html\n", global_context_program_name());
    printf("  %s package cache check elm/html --fix-broken # Re-download broken versions\n", global_context_program_name());
    printf("  %s package cache full-scan                 # Scan all packages in cache\n", global_context_program_name());
    printf("  %s package cache --from-url <url> elm/html # Download from URL to cache\n", global_context_program_name());
    printf("  %s package cache --from-file ./pkg elm/html # Download from local file to cache\n", global_context_program_name());
    printf("  %s package cache --major elm/html         # Download next major version\n", global_context_program_name());
    printf("\n");
    printf("Download Options:\n");
    printf("  <PACKAGE> <VERSION>             # Download specific version (e.g., 1.0.0)\n");
    printf("  --from-file <path> <package>    # Download from local file/directory to cache\n");
    printf("  --from-url <url> <package>      # Download from URL to cache\n");
    printf("  --major <package>               # Download next major version to cache\n");
    printf("  --ignore-hash                   # Skip SHA-1 hash verification\n");
    printf("  -v, --verbose                   # Show progress reports\n");
    printf("  -q, --quiet                     # Suppress progress reports\n");
    printf("  --help                          # Show this help\n");
    printf("\n");
    printf("Check Options:\n");
    printf("  --purge-broken                  # Remove broken directories without re-downloading\n");
    printf("  --fix-broken                    # Try to re-download broken versions\n");
    printf("\n");
    printf("Full-scan Options:\n");
    printf("  -q, --quiet                     # Only show summary counts\n");
    printf("  -v, --verbose                   # Show all issues including missing latest\n");
}

int cmd_cache(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        return cmd_cache_check(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "full-scan") == 0) {
        return cmd_cache_full_scan(argc - 1, argv + 1);
    }

    const char *package_arg = NULL;
    const char *version_arg = NULL;
    const char *from_file_path = NULL;
    const char *from_url = NULL;
    const char *major_package_name = NULL;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    bool major_upgrade = false;
    bool ignore_hash = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_cache_usage();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cmd_verbose = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cmd_quiet = true;
        } else if (strcmp(argv[i], "--ignore-hash") == 0) {
            ignore_hash = true;
        } else if (strcmp(argv[i], "--from-file") == 0) {
            if (i + 2 < argc) {
                i++;
                from_file_path = argv[i];
                i++;
                package_arg = argv[i];
            } else {
                fprintf(stderr, "Error: --from-file requires <path> and <package> arguments\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--from-url") == 0) {
            if (i + 2 < argc) {
                i++;
                from_url = argv[i];
                i++;
                package_arg = argv[i];
            } else {
                fprintf(stderr, "Error: --from-url requires <url> and <package> arguments\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                major_package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --major requires a package name\n");
                return 1;
            }
        } else if (argv[i][0] != '-') {
            if (!package_arg) {
                package_arg = argv[i];
            } else if (!version_arg) {
                version_arg = argv[i];
            } else {
                fprintf(stderr, "Error: Too many positional arguments\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (major_upgrade) {
        if (!major_package_name) {
            fprintf(stderr, "Error: --major requires a package name\n");
            return 1;
        }
        if (package_arg && strcmp(package_arg, major_package_name) != 0) {
            fprintf(stderr, "Error: Conflicting package names with --major\n");
            return 1;
        }
        package_arg = major_package_name;
    }

    if (from_file_path && from_url) {
        fprintf(stderr, "Error: Cannot use both --from-file and --from-url\n");
        return 1;
    }

    if (!package_arg) {
        fprintf(stderr, "Error: Package name is required\n");
        fprintf(stderr, "Usage: %s package cache <PACKAGE>\n", global_context_program_name());
        return 1;
    }

    LogLevel original_level = g_log_level;
    if (cmd_quiet) {
        if (g_log_level >= LOG_LEVEL_PROGRESS) {
            log_set_level(LOG_LEVEL_WARN);
        }
    } else if (cmd_verbose && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        log_set_level(original_level);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    env->ignore_hash = ignore_hash;

    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_arg, &author, &name)) {
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    int result = 0;
    char *version = NULL;
    CacheDownloadList *downloaded = NULL;

    if (from_file_path || from_url) {
        char *actual_author = NULL;
        char *actual_name = NULL;
        char *actual_version = NULL;
        char temp_dir_buf[1024];
        temp_dir_buf[0] = '\0';

        if (from_url) {
            snprintf(temp_dir_buf, sizeof(temp_dir_buf), "/tmp/wrap_cache_%s_%s", author, name);
            mkdir(temp_dir_buf, DIR_PERMISSIONS);

            char temp_file[1024];
            snprintf(temp_file, sizeof(temp_file), "%s/package.zip", temp_dir_buf);

            printf("Downloading from %s...\n", from_url);
            HttpResult http_result = http_download_file(env->curl_session, from_url, temp_file);
            if (http_result != HTTP_OK) {
                fprintf(stderr, "Error: Failed to download from URL: %s\n", http_result_to_string(http_result));
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            if (!extract_zip_selective(temp_file, temp_dir_buf)) {
                fprintf(stderr, "Error: Failed to extract archive\n");
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            unlink(temp_file);
            from_file_path = temp_dir_buf;
        }

        struct stat st;
        if (stat(from_file_path, &st) != 0) {
            fprintf(stderr, "Error: Path does not exist: %s\n", from_file_path);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        char elm_json_path[2048];
        if (S_ISDIR(st.st_mode)) {
            snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", from_file_path);
        } else {
            fprintf(stderr, "Error: --from-file requires a directory path\n");
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (stat(elm_json_path, &st) != 0) {
            char *found_path = find_package_elm_json(from_file_path);
            if (found_path) {
                snprintf(elm_json_path, sizeof(elm_json_path), "%s", found_path);
                arena_free(found_path);
            } else {
                fprintf(stderr, "Error: Could not find elm.json in %s\n", from_file_path);
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }
        }

        if (!read_package_info_from_elm_json(elm_json_path, &actual_author, &actual_name, &actual_version)) {
            fprintf(stderr, "Error: Could not read package information from %s\n", elm_json_path);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (strcmp(author, actual_author) != 0 || strcmp(name, actual_name) != 0) {
            printf("Warning: Package name in elm.json (%s/%s) differs from specified name (%s/%s)\n",
                   actual_author, actual_name, author, name);
        }

        arena_free(author);
        arena_free(name);
        author = actual_author;
        name = actual_name;
        version = actual_version;

        char source_dir[2048];
        char *elm_json_dir = strrchr(elm_json_path, '/');
        if (elm_json_dir) {
            *elm_json_dir = '\0';
            snprintf(source_dir, sizeof(source_dir), "%s", elm_json_path);
            *elm_json_dir = '/';
        } else {
            snprintf(source_dir, sizeof(source_dir), "%s", from_file_path);
        }

        if (!install_from_file(source_dir, env, author, name, version)) {
            fprintf(stderr, "Error: Failed to copy package to cache\n");
            arena_free(version);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        printf("Successfully cached %s/%s %s!\n", author, name, version);

        if (from_url && temp_dir_buf[0] != '\0') {
            remove_directory_recursive(temp_dir_buf);
        }

        result = 0;
    } else {
        RegistryEntry *registry_entry = registry_find(env->registry, author, name);
        if (!registry_entry) {
            log_error("I cannot find package '%s/%s'", author, name);
            log_error("Make sure the package name is correct");
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (registry_entry->version_count == 0) {
            log_error("Package %s/%s has no versions", author, name);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (version_arg) {
            Version requested = version_parse(version_arg);
            bool found = false;
            for (size_t i = 0; i < registry_entry->version_count; i++) {
                if (registry_version_compare(&registry_entry->versions[i], &requested) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                log_error("Version %s not found for package %s/%s", version_arg, author, name);
                log_error("Available versions:");
                size_t show_count = registry_entry->version_count < 10 ? registry_entry->version_count : 10;
                for (size_t i = 0; i < show_count; i++) {
                    char *v = version_to_string(&registry_entry->versions[i]);
                    if (v) {
                        log_error("  %s", v);
                        arena_free(v);
                    }
                }
                if (registry_entry->version_count > 10) {
                    log_error("  ... and %zu more", registry_entry->version_count - 10);
                }
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }
            version = arena_strdup(version_arg);
        } else if (major_upgrade) {
            version = version_to_string(&registry_entry->versions[0]);
        } else {
            version = version_to_string(&registry_entry->versions[0]);
        }

        if (!version) {
            log_error("Failed to get version for %s/%s", author, name);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        downloaded = cache_download_list_create();
        if (!downloaded) {
            log_error("Failed to create download list");
            arena_free(version);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        bool success = cache_download_package_recursive(env, author, name, version, downloaded);

        if (success) {
            if (downloaded->count > 0) {
                printf("\nDownloaded %zu package%s to cache:\n", downloaded->count, downloaded->count == 1 ? "" : "s");
                for (size_t i = 0; i < downloaded->count; i++) {
                    printf("  %s\n", downloaded->packages[i]);
                }
            } else {
                printf("Package %s/%s %s and all dependencies already cached\n", author, name, version);
            }
            result = 0;
        } else {
            result = 1;
        }

        cache_download_list_free(downloaded);
    }

    if (version) arena_free(version);
    arena_free(author);
    arena_free(name);
    install_env_free(env);
    log_set_level(original_level);

    return result;
}
