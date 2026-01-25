#include "cache_download_missing.h"
#include "../cache_common.h"
#include "../../../elm_json.h"
#include "../../../cache.h"
#include "../../../install_env.h"
#include "../../../registry.h"
#include "../../../global_context.h"
#include "../../../alloc.h"
#include "../../../constants.h"
#include "../../../shared/log.h"
#include "../../package/package_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

/* Download source modes */
typedef enum {
    DOWNLOAD_SOURCE_GITHUB,    /* Direct GitHub download (default) */
    DOWNLOAD_SOURCE_REGISTRY   /* Via package registry */
} DownloadSource;

/* Entry representing a package to download */
typedef struct {
    char *author;
    char *name;
    char *version;
} MissingPackage;

/* List of missing packages */
typedef struct {
    MissingPackage *packages;
    size_t count;
    size_t capacity;
} MissingPackageList;

static MissingPackageList* missing_list_create(void) {
    MissingPackageList *list = arena_malloc(sizeof(MissingPackageList));
    if (!list) return NULL;

    list->capacity = INITIAL_SMALL_CAPACITY;
    list->count = 0;
    list->packages = arena_malloc(sizeof(MissingPackage) * list->capacity);
    if (!list->packages) {
        arena_free(list);
        return NULL;
    }
    return list;
}

static bool missing_list_add(MissingPackageList *list, const char *author, const char *name, const char *version) {
    if (!list || !author || !name || !version) return false;

    /* Check for duplicates */
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->packages[i].author, author) == 0 &&
            strcmp(list->packages[i].name, name) == 0 &&
            strcmp(list->packages[i].version, version) == 0) {
            return true; /* Already in list */
        }
    }

    if (list->count >= list->capacity) {
        list->capacity *= 2;
        MissingPackage *new_packages = arena_realloc(list->packages, sizeof(MissingPackage) * list->capacity);
        if (!new_packages) return false;
        list->packages = new_packages;
    }

    list->packages[list->count].author = arena_strdup(author);
    list->packages[list->count].name = arena_strdup(name);
    list->packages[list->count].version = arena_strdup(version);
    list->count++;

    return true;
}

static void missing_list_free(MissingPackageList *list) {
    if (!list) return;

    if (list->packages) {
        for (size_t i = 0; i < list->count; i++) {
            if (list->packages[i].author) arena_free(list->packages[i].author);
            if (list->packages[i].name) arena_free(list->packages[i].name);
            if (list->packages[i].version) arena_free(list->packages[i].version);
        }
        arena_free(list->packages);
    }
    arena_free(list);
}

/* Check a PackageMap and add missing packages to the list (registry mode) */
static bool check_package_map_registry(
    PackageMap *map,
    InstallEnv *env,
    MissingPackageList *missing,
    bool is_package_project
) {
    if (!map) return true;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!pkg->author || !pkg->name || !pkg->version) continue;

        char *resolved_version = NULL;

        if (is_package_project && version_is_constraint(pkg->version)) {
            /* For package projects with version constraints, resolve to latest matching */
            Version resolved;
            if (!registry_resolve_constraint(env->registry, pkg->author, pkg->name, pkg->version, &resolved)) {
                log_error("Could not resolve constraint '%s' for %s/%s",
                         pkg->version, pkg->author, pkg->name);
                return false;
            }
            resolved_version = version_to_string(&resolved);
            if (!resolved_version) {
                log_error("Failed to format resolved version for %s/%s", pkg->author, pkg->name);
                return false;
            }
        } else {
            /* For applications with exact versions, use as-is */
            resolved_version = arena_strdup(pkg->version);
            if (!resolved_version) {
                log_error("Failed to allocate version string");
                return false;
            }
        }

        /* Check if package is in cache */
        if (!cache_package_fully_downloaded(env->cache, pkg->author, pkg->name, resolved_version)) {
            if (!missing_list_add(missing, pkg->author, pkg->name, resolved_version)) {
                arena_free(resolved_version);
                return false;
            }
        }

        arena_free(resolved_version);
    }

    return true;
}

/* Check a PackageMap and add missing packages to the list (GitHub mode - exact versions only) */
static bool check_package_map_github(
    PackageMap *map,
    CacheConfig *cache,
    MissingPackageList *missing
) {
    if (!map) return true;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!pkg->author || !pkg->name || !pkg->version) continue;

        /* Check if package is in cache */
        if (!cache_package_fully_downloaded(cache, pkg->author, pkg->name, pkg->version)) {
            if (!missing_list_add(missing, pkg->author, pkg->name, pkg->version)) {
                return false;
            }
        }
    }

    return true;
}

/* Try to resolve elm.json path from argument (directory or file path) */
static bool try_resolve_elm_json_path(const char *arg, char **out_elm_json_path) {
    if (!arg || !out_elm_json_path) {
        return false;
    }

    struct stat st;
    char path_buf[MAX_PATH_LENGTH];

    if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(path_buf, sizeof(path_buf), "%s/elm.json", arg);
    } else if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(path_buf, sizeof(path_buf), "%s", arg);
    } else {
        log_error("Path does not exist: %s", arg);
        return false;
    }

    if (stat(path_buf, &st) != 0 || !S_ISREG(st.st_mode)) {
        log_error("elm.json not found at: %s", path_buf);
        return false;
    }

    *out_elm_json_path = arena_strdup(path_buf);
    return *out_elm_json_path != NULL;
}

static void print_download_missing_usage(void) {
    const char *prog = global_context_program_name();
    printf("Usage: %s package cache missing [OPTIONS] [PATH]\n", prog);
    printf("\n");
    printf("Download missing dependencies from elm.json to the cache.\n");
    printf("\n");
    printf("Reads elm.json and identifies which dependencies are not yet downloaded.\n");
    printf("By default, downloads directly from GitHub for faster operation.\n");
    printf("\n");
    printf("Download Sources:\n");
    printf("  (default)         Download directly from GitHub\n");
    printf("  --from-github     Same as default (explicit flag for clarity)\n");
    printf("  --from-registry   Use package registry for metadata and download\n");
    printf("                    (required for package projects with version constraints)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes         Skip confirmation prompt and download immediately\n");
    printf("  -v, --verbose     Show detailed progress during download\n");
    printf("  -q, --quiet       Suppress progress messages\n");
    printf("  --help            Show this help message\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PATH              Optional path to directory containing elm.json\n");
    printf("                    (defaults to current directory)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package cache missing              # Download from GitHub (default)\n", prog);
    printf("  %s package cache missing -y           # Download without prompting\n", prog);
    printf("  %s package cache missing ./my-app     # Specify project path\n", prog);
    printf("  %s package cache missing --from-registry  # Use registry (for packages)\n", prog);
}

int cmd_cache_download_missing(int argc, char *argv[]) {
    bool auto_confirm = false;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    DownloadSource source = DOWNLOAD_SOURCE_GITHUB;  /* Default to GitHub */
    const char *path_arg = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_download_missing_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_confirm = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cmd_verbose = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cmd_quiet = true;
        } else if (strcmp(argv[i], "--from-registry") == 0) {
            source = DOWNLOAD_SOURCE_REGISTRY;
        } else if (strcmp(argv[i], "--from-github") == 0) {
            source = DOWNLOAD_SOURCE_GITHUB;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_download_missing_usage();
            return 1;
        } else {
            /* Positional argument - should be PATH */
            if (path_arg) {
                fprintf(stderr, "Error: Only one path argument is allowed\n");
                print_download_missing_usage();
                return 1;
            }
            path_arg = argv[i];
        }
    }

    /* Resolve elm.json path */
    char *elm_json_path = NULL;
    if (path_arg) {
        if (!try_resolve_elm_json_path(path_arg, &elm_json_path)) {
            return 1;
        }
    } else {
        /* Check for elm.json in current directory */
        if (access(ELM_JSON_PATH, F_OK) != 0) {
            fprintf(stderr, "Error: No elm.json found in current directory\n");
            fprintf(stderr, "Run this command from a directory containing an Elm project,\n");
            fprintf(stderr, "or specify the path: %s package cache missing ./path/to/project\n",
                    global_context_program_name());
            return 1;
        }
        elm_json_path = arena_strdup(ELM_JSON_PATH);
        if (!elm_json_path) {
            log_error("Failed to allocate path string");
            return 1;
        }
    }

    /* Set log level */
    LogLevel original_level = g_log_level;
    if (cmd_quiet) {
        if (g_log_level >= LOG_LEVEL_PROGRESS) {
            log_set_level(LOG_LEVEL_WARN);
        }
    } else if (cmd_verbose && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    /* Read elm.json */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        fprintf(stderr, "Error: Failed to read elm.json at: %s\n", elm_json_path);
        arena_free(elm_json_path);
        log_set_level(original_level);
        return 1;
    }

    bool is_package_project = (elm_json->type == ELM_PROJECT_PACKAGE);

    /* For package projects with version constraints, require --from-registry */
    if (is_package_project && source == DOWNLOAD_SOURCE_GITHUB) {
        fprintf(stderr, "Error: Package projects require --from-registry flag\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Package projects use version constraints (e.g., \"1.0.0 <= v < 2.0.0\")\n");
        fprintf(stderr, "which need the registry to resolve to exact versions.\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Run: %s package cache missing --from-registry\n", global_context_program_name());
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        log_set_level(original_level);
        return 1;
    }

    /* Create install environment based on download source */
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        log_set_level(original_level);
        return 1;
    }

    if (source == DOWNLOAD_SOURCE_REGISTRY) {
        /* Full initialization with registry */
        if (!install_env_init(env)) {
            log_error("Failed to initialize install environment");
            install_env_free(env);
            elm_json_free(elm_json);
            arena_free(elm_json_path);
            log_set_level(original_level);
            return 1;
        }
    } else {
        /* Lightweight initialization for GitHub-only download */
        if (!install_env_prepare_v1(env)) {
            log_error("Failed to initialize install environment");
            install_env_free(env);
            elm_json_free(elm_json);
            arena_free(elm_json_path);
            log_set_level(original_level);
            return 1;
        }
    }

    /* Create list for missing packages */
    MissingPackageList *missing = missing_list_create();
    if (!missing) {
        log_error("Failed to create missing package list");
        install_env_free(env);
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        log_set_level(original_level);
        return 1;
    }

    bool success = true;

    if (source == DOWNLOAD_SOURCE_REGISTRY) {
        /* Registry mode: use existing logic with version constraint resolution */
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            if (!check_package_map_registry(elm_json->dependencies_direct, env, missing, false)) {
                success = false;
            }
            if (success && !check_package_map_registry(elm_json->dependencies_indirect, env, missing, false)) {
                success = false;
            }
            if (success && !check_package_map_registry(elm_json->dependencies_test_direct, env, missing, false)) {
                success = false;
            }
            if (success && !check_package_map_registry(elm_json->dependencies_test_indirect, env, missing, false)) {
                success = false;
            }
        } else {
            if (!check_package_map_registry(elm_json->package_dependencies, env, missing, is_package_project)) {
                success = false;
            }
            if (success && !check_package_map_registry(elm_json->package_test_dependencies, env, missing, is_package_project)) {
                success = false;
            }
        }
    } else {
        /* GitHub mode: direct download with exact versions */
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            if (!check_package_map_github(elm_json->dependencies_direct, env->cache, missing)) {
                success = false;
            }
            if (success && !check_package_map_github(elm_json->dependencies_indirect, env->cache, missing)) {
                success = false;
            }
            if (success && !check_package_map_github(elm_json->dependencies_test_direct, env->cache, missing)) {
                success = false;
            }
            if (success && !check_package_map_github(elm_json->dependencies_test_indirect, env->cache, missing)) {
                success = false;
            }
        }
        /* Note: package projects are rejected earlier, so no else branch needed */
    }

    if (!success) {
        log_error("Failed to check dependencies");
        missing_list_free(missing);
        install_env_free(env);
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        log_set_level(original_level);
        return 1;
    }

    /* Report findings */
    if (missing->count == 0) {
        printf("All dependencies are already cached.\n");
        missing_list_free(missing);
        install_env_free(env);
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        log_set_level(original_level);
        return 0;
    }

    /* Print the plan */
    printf("The following %zu package%s will be downloaded:\n\n",
           missing->count, missing->count == 1 ? "" : "s");

    for (size_t i = 0; i < missing->count; i++) {
        printf("  %s/%s %s\n",
               missing->packages[i].author,
               missing->packages[i].name,
               missing->packages[i].version);
    }
    printf("\n");

    if (source == DOWNLOAD_SOURCE_GITHUB) {
        printf("Source: GitHub (direct download)\n\n");
    } else {
        printf("Source: Package registry\n\n");
    }

    /* Ask for confirmation unless -y was passed */
    if (!auto_confirm) {
        printf("Proceed with download? [Y/n] ");
        fflush(stdout);

        char response[MAX_RESPONSE_LENGTH];
        if (fgets(response, sizeof(response), stdin) == NULL) {
            printf("Cancelled.\n");
            missing_list_free(missing);
            install_env_free(env);
            elm_json_free(elm_json);
            arena_free(elm_json_path);
            log_set_level(original_level);
            return 0;
        }

        /* Trim newline */
        size_t len = strlen(response);
        if (len > 0 && response[len - 1] == '\n') {
            response[len - 1] = '\0';
        }

        /* Check response - empty or y/Y means yes */
        if (response[0] != '\0' &&
            response[0] != 'y' && response[0] != 'Y') {
            printf("Cancelled.\n");
            missing_list_free(missing);
            install_env_free(env);
            elm_json_free(elm_json);
            arena_free(elm_json_path);
            log_set_level(original_level);
            return 0;
        }
    }

    /* Download missing packages */
    int result = 0;
    size_t downloaded = 0;

    for (size_t i = 0; i < missing->count; i++) {
        MissingPackage *pkg = &missing->packages[i];

        log_progress("Downloading %s/%s %s...", pkg->author, pkg->name, pkg->version);

        bool download_success = false;

        if (source == DOWNLOAD_SOURCE_GITHUB) {
            char error_msg[MAX_ERROR_MESSAGE_LENGTH];
            download_success = cache_download_from_github(env, pkg->author, pkg->name, pkg->version,
                                                          cmd_verbose, error_msg, sizeof(error_msg));
            if (!download_success) {
                fprintf(stderr, "Error: Failed to download %s/%s %s: %s\n",
                       pkg->author, pkg->name, pkg->version,
                       error_msg[0] ? error_msg : "unknown error");
            }
        } else {
            download_success = install_env_download_package(env, pkg->author, pkg->name, pkg->version);
            if (!download_success) {
                fprintf(stderr, "Error: Failed to download %s/%s %s\n",
                       pkg->author, pkg->name, pkg->version);
            }
        }

        if (!download_success) {
            result = 1;
            break;
        }

        downloaded++;
    }

    if (result == 0) {
        printf("\nSuccessfully downloaded %zu package%s to cache.\n",
               downloaded, downloaded == 1 ? "" : "s");
    } else {
        fprintf(stderr, "\nDownloaded %zu of %zu packages before failure.\n",
               downloaded, missing->count);
    }

    missing_list_free(missing);
    install_env_free(env);
    elm_json_free(elm_json);
    arena_free(elm_json_path);
    log_set_level(original_level);

    return result;
}
