#include "package_common.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../feature_flags.h"
#include "../../elm_json.h"
#include "../../cache.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../http_client.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include "../../fileutil.h"
#include "../../dyn_array.h"
#include "../../commands/cache/check/cache_check.h"
#include "../../commands/cache/full_scan/cache_full_scan.h"
#include "../../commands/cache/download_missing/cache_download_missing.h"
#include "../../commands/cache/download_all/cache_download_all.h"
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

static void free_package_specs(PackageInstallSpec *specs, int count) {
    if (!specs) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (specs[i].author) {
            arena_free(specs[i].author);
            specs[i].author = NULL;
        }
        if (specs[i].name) {
            arena_free(specs[i].name);
            specs[i].name = NULL;
        }
    }
    arena_free(specs);
}

static CacheDownloadList* cache_download_list_create(void) {
    CacheDownloadList *list = arena_malloc(sizeof(CacheDownloadList));
    if (!list) return NULL;
    list->capacity = INITIAL_SMALL_CAPACITY;
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
        log_error("Failed to download %s/%s %s", author, name, version);
        return false;
    }

    cache_download_list_add(downloaded, author, name, version);

    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        log_error("Failed to get package path for %s/%s %s", author, name, version);
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
    LogLevel old_level = g_log_level;
    log_set_level(LOG_LEVEL_PROGRESS);
    const char *prog = global_context_program_name();
    log_progress("Usage:");
    log_progress("  %s package cache [OPTIONS] PACKAGE[@VERSION] [PACKAGE[@VERSION]...]", prog);
    log_progress("  %s package cache check PACKAGE [OPTIONS]", prog);
    log_progress("  %s package cache full-scan [OPTIONS]", prog);
    log_progress("  %s package cache missing [OPTIONS]", prog);
    if (feature_cache_download_all_enabled()) {
        log_progress("  %s package cache download-all [OPTIONS]", prog);
    }
    log_progress("");
    log_progress("Download packages into the cache so installs can run offline.");
    log_progress("");
    log_progress("Examples:");
    log_progress("  %s package cache elm/html                    # Download latest elm/html", prog);
    log_progress("  %s package cache elm/html@1.0.0              # Download specific version", prog);
    log_progress("  %s package cache elm/html elm/json           # Cache multiple packages", prog);
    log_progress("  %s package cache elm/html 1.0.0 elm/json     # Mix positional version + latest", prog);
    log_progress("  %s package cache check elm/html              # Check cache status for elm/html", prog);
    log_progress("  %s package cache check elm/html --fix-broken # Re-download broken versions", prog);
    log_progress("  %s package cache full-scan                   # Scan all packages in cache", prog);
    log_progress("  %s package cache missing                     # Download missing deps from GitHub", prog);
    log_progress("  %s package cache missing ./my-app            # Specify project path", prog);
    log_progress("  %s package cache missing --from-registry     # Use registry (for packages)", prog);
    if (feature_cache_download_all_enabled()) {
        log_progress("  %s package cache download-all                # Download entire registry to cache", prog);
        log_progress("  %s package cache download-all --latest-only  # Only latest version of each package", prog);
    }
    log_progress("  %s package cache --from-file ./pkg elm/html  # Download from local directory", prog);
    log_progress("  %s package cache --from-url URL elm/html     # Download from URL to cache", prog);
    log_progress("  %s package cache --major elm/html            # Download highest major version", prog);
    log_progress("");
    log_progress("Download Options:");
    log_progress("  PACKAGE[@VERSION] [PACKAGE[@VERSION]...]   # One or more packages (use @VERSION for specific release)");
    log_progress("  PACKAGE VERSION                           # Backwards-compatible positional version (single package)");
    log_progress("  --from-file PATH PACKAGE[@VERSION]        # Download from local directory/archive (single package)");
    log_progress("  --from-url URL PACKAGE[@VERSION]          # Download from URL to cache (single package)");
    log_progress("  --major PACKAGE                           # Download highest available major version (single package)");
    log_progress("  --ignore-hash                             # Skip SHA-1 hash verification");
    log_progress("  -v, --verbose                             # Show progress reports");
    log_progress("  -q, --quiet                               # Suppress progress reports");
    log_progress("  --help                                    # Show this help");
    log_progress("");
    log_progress("Check Options:");
    log_progress("  --purge-broken                            # Remove broken directories without re-downloading");
    log_progress("  --fix-broken                              # Try to re-download broken versions");
    log_progress("");
    log_progress("Full-scan Options:");
    log_progress("  -q, --quiet                               # Only show summary counts");
    log_progress("  -v, --verbose                             # Show all issues including missing latest");
    if (feature_cache_download_all_enabled()) {
        log_progress("");
        log_progress("Download-all Options:");
        log_progress("  -y, --yes                                 # Skip confirmation prompt");
        log_progress("  --dry-run                                 # Show what would be downloaded");
        log_progress("  --latest-only                             # Only latest version of each package");
    }
    log_set_level(old_level);
}

int cmd_cache(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        return cmd_cache_check(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "full-scan") == 0) {
        return cmd_cache_full_scan(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "missing") == 0) {
        return cmd_cache_download_missing(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "download-all") == 0) {
        if (!feature_cache_download_all_enabled()) {
            fprintf(stderr, "Error: Command 'cache download-all' is not available in this build.\n");
            return 1;
        }
        return cmd_cache_download_all(argc - 1, argv + 1);
    }
    const char *from_file_path = NULL;
    const char *from_url = NULL;
    const char *major_package_name = NULL;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    bool major_upgrade = false;
    bool ignore_hash = false;

    PackageInstallSpec *specs = NULL;
    int specs_count = 0;
    int specs_capacity = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_cache_usage();
            free_package_specs(specs, specs_count);
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
                char *author = NULL;
                char *name = NULL;
                Version version = {0};
                bool has_version = false;
                if (strchr(argv[i], '@')) {
                    if (!parse_package_with_version(argv[i], &author, &name, &version)) {
                        fprintf(stderr, "Error: Invalid package specification '%s'\n", argv[i]);
                        print_cache_usage();
                        free_package_specs(specs, specs_count);
                        return 1;
                    }
                    has_version = true;
                } else {
                    if (!parse_package_name(argv[i], &author, &name)) {
                        print_cache_usage();
                        free_package_specs(specs, specs_count);
                        return 1;
                    }
                }
                DYNARRAY_PUSH(
                    specs,
                    specs_count,
                    specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = version,
                        .has_version = has_version
                    }),
                    PackageInstallSpec
                );
            } else {
                fprintf(stderr, "Error: --from-file requires PATH and PACKAGE arguments\n");
                free_package_specs(specs, specs_count);
                return 1;
            }
        } else if (strcmp(argv[i], "--from-url") == 0) {
            if (i + 2 < argc) {
                i++;
                from_url = argv[i];
                i++;
                char *author = NULL;
                char *name = NULL;
                Version version = {0};
                bool has_version = false;
                if (strchr(argv[i], '@')) {
                    if (!parse_package_with_version(argv[i], &author, &name, &version)) {
                        fprintf(stderr, "Error: Invalid package specification '%s'\n", argv[i]);
                        print_cache_usage();
                        free_package_specs(specs, specs_count);
                        return 1;
                    }
                    has_version = true;
                } else {
                    if (!parse_package_name(argv[i], &author, &name)) {
                        print_cache_usage();
                        free_package_specs(specs, specs_count);
                        return 1;
                    }
                }
                DYNARRAY_PUSH(
                    specs,
                    specs_count,
                    specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = version,
                        .has_version = has_version
                    }),
                    PackageInstallSpec
                );
            } else {
                fprintf(stderr, "Error: --from-url requires URL and PACKAGE arguments\n");
                free_package_specs(specs, specs_count);
                return 1;
            }
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                major_package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --major requires a package name\n");
                print_cache_usage();
                free_package_specs(specs, specs_count);
                return 1;
            }
        } else if (argv[i][0] != '-') {
            Version parsed_version;
            if (specs_count > 0 &&
                !specs[specs_count - 1].has_version &&
                version_parse_safe(argv[i], &parsed_version)) {
                specs[specs_count - 1].version = parsed_version;
                specs[specs_count - 1].has_version = true;
            } else if (strchr(argv[i], '@')) {
                char *author = NULL;
                char *name = NULL;
                Version version = {0};
                if (!parse_package_with_version(argv[i], &author, &name, &version)) {
                    fprintf(stderr, "Error: Invalid package specification '%s'\n", argv[i]);
                    print_cache_usage();
                    free_package_specs(specs, specs_count);
                    return 1;
                }
                DYNARRAY_PUSH(
                    specs,
                    specs_count,
                    specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = version,
                        .has_version = true
                    }),
                    PackageInstallSpec
                );
            } else {
                char *author = NULL;
                char *name = NULL;
                if (!parse_package_name(argv[i], &author, &name)) {
                    print_cache_usage();
                    free_package_specs(specs, specs_count);
                    return 1;
                }
                DYNARRAY_PUSH(
                    specs,
                    specs_count,
                    specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = {0},
                        .has_version = false
                    }),
                    PackageInstallSpec
                );
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_cache_usage();
            free_package_specs(specs, specs_count);
            return 1;
        }
    }

    if (major_upgrade) {
        if (!major_package_name) {
            fprintf(stderr, "Error: --major requires a package name\n");
            print_cache_usage();
            free_package_specs(specs, specs_count);
            return 1;
        }
        if (specs_count > 0) {
            fprintf(stderr, "Error: --major can only be used with a single package\n");
            free_package_specs(specs, specs_count);
            return 1;
        }
        char *author = NULL;
        char *name = NULL;
        Version version = {0};
        bool has_version = false;
        if (strchr(major_package_name, '@')) {
            if (!parse_package_with_version(major_package_name, &author, &name, &version)) {
                fprintf(stderr, "Error: Invalid package specification '%s'\n", major_package_name);
                print_cache_usage();
                free_package_specs(specs, specs_count);
                return 1;
            }
            has_version = true;
            /* Warn when both --major and explicit version are specified */
            fprintf(stderr, "Warning: --major flag is ignored when an explicit version is specified\n");
            fprintf(stderr, "         Caching %s/%s at version %u.%u.%u\n",
                    author, name, version.major, version.minor, version.patch);
        } else {
            if (!parse_package_name(major_package_name, &author, &name)) {
                print_cache_usage();
                free_package_specs(specs, specs_count);
                return 1;
            }
        }
        DYNARRAY_PUSH(
            specs,
            specs_count,
            specs_capacity,
            ((PackageInstallSpec){
                .author = author,
                .name = name,
                .version = version,
                .has_version = has_version
            }),
            PackageInstallSpec
        );
    }

    if (from_file_path && from_url) {
        fprintf(stderr, "Error: Cannot use both --from-file and --from-url\n");
        free_package_specs(specs, specs_count);
        return 1;
    }

    if ((from_file_path || from_url) && specs_count != 1) {
        fprintf(stderr, "Error: %s can only cache one package at a time\n",
                from_file_path ? "--from-file" : "--from-url");
        free_package_specs(specs, specs_count);
        return 1;
    }

    if (!from_file_path && !from_url && specs_count == 0) {
        fprintf(stderr, "Error: At least one package is required\n");
        print_cache_usage();
        free_package_specs(specs, specs_count);
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
        free_package_specs(specs, specs_count);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        log_set_level(original_level);
        free_package_specs(specs, specs_count);
        return 1;
    }

    env->ignore_hash = ignore_hash;

    int result = 0;

    if (from_file_path || from_url) {
        PackageInstallSpec *spec = &specs[0];
        char *author = spec->author;
        char *name = spec->name;
        char *version = NULL;
        char *actual_author = NULL;
        char *actual_name = NULL;
        char *actual_version = NULL;
        char temp_dir_buf[MAX_TEMP_PATH_LENGTH];
        temp_dir_buf[0] = '\0';

        if (from_url) {
            snprintf(temp_dir_buf, sizeof(temp_dir_buf), "/tmp/wrap_cache_%s_%s", author, name);
            mkdir(temp_dir_buf, DIR_PERMISSIONS);

            char temp_file[MAX_TEMP_PATH_LENGTH];
            snprintf(temp_file, sizeof(temp_file), "%s/package.zip", temp_dir_buf);

            printf("Downloading from %s...\n", from_url);
            HttpResult http_result = http_download_file(env->curl_session, from_url, temp_file);
            if (http_result != HTTP_OK) {
                fprintf(stderr, "Error: Failed to download from URL: %s\n", http_result_to_string(http_result));
                install_env_free(env);
                log_set_level(original_level);
                free_package_specs(specs, specs_count);
                return 1;
            }

            if (!extract_zip_selective(temp_file, temp_dir_buf)) {
                fprintf(stderr, "Error: Failed to extract archive\n");
                install_env_free(env);
                log_set_level(original_level);
                free_package_specs(specs, specs_count);
                return 1;
            }

            unlink(temp_file);
            from_file_path = temp_dir_buf;
        }

        struct stat st;
        if (stat(from_file_path, &st) != 0) {
            fprintf(stderr, "Error: Path does not exist: %s\n", from_file_path);
            install_env_free(env);
            log_set_level(original_level);
            free_package_specs(specs, specs_count);
            return 1;
        }

        char elm_json_path[MAX_PATH_LENGTH];
        if (S_ISDIR(st.st_mode)) {
            snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", from_file_path);
        } else {
            fprintf(stderr, "Error: --from-file requires a directory path\n");
            install_env_free(env);
            log_set_level(original_level);
            free_package_specs(specs, specs_count);
            return 1;
        }

        if (stat(elm_json_path, &st) != 0) {
            char *found_path = find_package_elm_json(from_file_path);
            if (found_path) {
                snprintf(elm_json_path, sizeof(elm_json_path), "%s", found_path);
                arena_free(found_path);
            } else {
                fprintf(stderr, "Error: Could not find elm.json in %s\n", from_file_path);
                install_env_free(env);
                log_set_level(original_level);
                free_package_specs(specs, specs_count);
                return 1;
            }
        }

        if (!read_package_info_from_elm_json(elm_json_path, &actual_author, &actual_name, &actual_version)) {
            fprintf(stderr, "Error: Could not read package information from %s\n", elm_json_path);
            install_env_free(env);
            log_set_level(original_level);
            free_package_specs(specs, specs_count);
            return 1;
        }

        if (strcmp(author, actual_author) != 0 || strcmp(name, actual_name) != 0) {
            printf("Warning: Package name in elm.json (%s/%s) differs from specified name (%s/%s)\n",
                   actual_author, actual_name, author, name);
        }

        if (spec->has_version) {
            char *spec_version = version_to_string(&spec->version);
            if (!spec_version || strcmp(spec_version, actual_version) != 0) {
                fprintf(stderr, "Error: Specified version does not match elm.json (%s vs %s)\n",
                        spec_version ? spec_version : "(invalid)", actual_version);
                if (spec_version) arena_free(spec_version);
                arena_free(actual_author);
                arena_free(actual_name);
                arena_free(actual_version);
                install_env_free(env);
                log_set_level(original_level);
                free_package_specs(specs, specs_count);
                return 1;
            }
            arena_free(spec_version);
        }

        arena_free(spec->author);
        arena_free(spec->name);
        spec->author = actual_author;
        spec->name = actual_name;
        author = spec->author;
        name = spec->name;
        version = actual_version;

        char source_dir[MAX_PATH_LENGTH];
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
            install_env_free(env);
            log_set_level(original_level);
            free_package_specs(specs, specs_count);
            return 1;
        }

        printf("Successfully cached %s/%s %s!\n", author, name, version);

        if (from_url && temp_dir_buf[0] != '\0') {
            remove_directory_recursive(temp_dir_buf);
        }

        arena_free(version);
    } else {
        for (int i = 0; i < specs_count; i++) {
            PackageInstallSpec *spec = &specs[i];
            RegistryEntry *registry_entry = registry_find(env->registry, spec->author, spec->name);
            if (!registry_entry) {
                log_error("I cannot find package '%s/%s'", spec->author, spec->name);
                log_error("Make sure the package name is correct");
                result = 1;
                break;
            }

            if (registry_entry->version_count == 0) {
                log_error("Package %s/%s has no versions", spec->author, spec->name);
                result = 1;
                break;
            }

            Version selected_version = registry_entry->versions[0];
            if (spec->has_version) {
                bool found = false;
                for (size_t j = 0; j < registry_entry->version_count; j++) {
                    if (version_compare(&registry_entry->versions[j], &spec->version) == 0) {
                        found = true;
                        selected_version = spec->version;
                        break;
                    }
                }
                if (!found) {
                    char *requested = version_to_string(&spec->version);
                    log_error("Version %s not found for package %s/%s",
                              requested ? requested : "(invalid)",
                              spec->author,
                              spec->name);
                    if (requested) arena_free(requested);
                    log_error("Available versions:");
                    size_t show_count = registry_entry->version_count < 10 ? registry_entry->version_count : 10;
                    for (size_t j = 0; j < show_count; j++) {
                        char *v = version_to_string(&registry_entry->versions[j]);
                        if (v) {
                            log_error("  %s", v);
                            arena_free(v);
                        }
                    }
                    if (registry_entry->version_count > 10) {
                        log_error("  ... and %zu more", registry_entry->version_count - 10);
                    }
                    result = 1;
                    break;
                }
            }

            char *version_str = version_to_string(&selected_version);
            if (!version_str) {
                log_error("Failed to format version for %s/%s", spec->author, spec->name);
                result = 1;
                break;
            }

            CacheDownloadList *downloaded = cache_download_list_create();
            if (!downloaded) {
                log_error("Failed to create download list");
                arena_free(version_str);
                result = 1;
                break;
            }

            bool success = cache_download_package_recursive(env, spec->author, spec->name, version_str, downloaded);

            if (success) {
                if (downloaded->count > 0) {
                    printf("\nDownloaded %zu package%s to cache for %s/%s %s:\n",
                           downloaded->count,
                           downloaded->count == 1 ? "" : "s",
                           spec->author,
                           spec->name,
                           version_str);
                    for (size_t j = 0; j < downloaded->count; j++) {
                        printf("  %s\n", downloaded->packages[j]);
                    }
                } else {
                    printf("Package %s/%s %s and all dependencies already cached\n",
                           spec->author,
                           spec->name,
                           version_str);
                }
            } else {
                result = 1;
                cache_download_list_free(downloaded);
                arena_free(version_str);
                break;
            }

            cache_download_list_free(downloaded);
            arena_free(version_str);
        }
    }

    install_env_free(env);
    log_set_level(original_level);
    free_package_specs(specs, specs_count);
    return result;
}
