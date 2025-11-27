#include "cache_check.h"
#include "../../../cache.h"
#include "../../../registry.h"
#include "../../../install_env.h"
#include "../../../alloc.h"
#include "../../../log.h"
#include "../../../progname.h"
#include "../../../fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* ANSI color codes */
#define ANSI_GREEN "\033[32m"
#define ANSI_RED "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN "\033[36m"
#define ANSI_RESET "\033[0m"

/* Status of a cached package version */
typedef enum {
    PKG_STATUS_OK,        /* Has src/ directory with content */
    PKG_STATUS_BROKEN,    /* Missing or empty src/ directory */
    PKG_STATUS_NOT_CACHED /* Not in cache */
} PackageStatus;

/* Version status entry */
typedef struct {
    char *version_str;
    PackageStatus status;
    bool in_registry;
} VersionStatus;

/* Parse package name into author and name components */
static bool parse_package_name(const char *package, char **author, char **name) {
    if (!package) return false;

    const char *slash = strchr(package, '/');
    if (!slash) {
        fprintf(stderr, "Error: Package name must be in format 'author/package'\n");
        return false;
    }

    size_t author_len = slash - package;
    *author = arena_malloc(author_len + 1);
    if (!*author) return false;
    strncpy(*author, package, author_len);
    (*author)[author_len] = '\0';

    *name = arena_strdup(slash + 1);
    if (!*name) {
        arena_free(*author);
        return false;
    }

    return true;
}

/* Check if a directory is empty */
static bool is_directory_empty(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return true;

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        count++;
        break;
    }
    closedir(dir);

    return count == 0;
}

/* Get the status of a cached package version */
static PackageStatus get_package_status(CacheConfig *cache, const char *author, 
                                         const char *name, const char *version) {
    char *pkg_path = cache_get_package_path(cache, author, name, version);
    if (!pkg_path) return PKG_STATUS_NOT_CACHED;

    struct stat st;
    if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        arena_free(pkg_path);
        return PKG_STATUS_NOT_CACHED;
    }

    /* Check if src/ directory exists and has content */
    size_t src_len = strlen(pkg_path) + strlen("/src") + 1;
    char *src_path = arena_malloc(src_len);
    if (!src_path) {
        arena_free(pkg_path);
        return PKG_STATUS_NOT_CACHED;
    }

    snprintf(src_path, src_len, "%s/src", pkg_path);

    PackageStatus status;
    if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        status = PKG_STATUS_BROKEN;
    } else if (is_directory_empty(src_path)) {
        status = PKG_STATUS_BROKEN;
    } else {
        status = PKG_STATUS_OK;
    }

    arena_free(src_path);
    arena_free(pkg_path);
    return status;
}

/* List cached versions for a package from the cache directory */
static char** list_cached_versions(CacheConfig *cache, const char *author, 
                                   const char *name, size_t *count) {
    *count = 0;

    /* Build path to package directory: packages_dir/author/name */
    size_t dir_len = strlen(cache->packages_dir) + strlen(author) + strlen(name) + 3;
    char *pkg_dir = arena_malloc(dir_len);
    if (!pkg_dir) return NULL;

    snprintf(pkg_dir, dir_len, "%s/%s/%s", cache->packages_dir, author, name);

    struct stat st;
    if (stat(pkg_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        arena_free(pkg_dir);
        return NULL;
    }

    DIR *dir = opendir(pkg_dir);
    if (!dir) {
        arena_free(pkg_dir);
        return NULL;
    }

    /* Count entries first */
    size_t capacity = 16;
    char **versions = arena_malloc(sizeof(char*) * capacity);
    if (!versions) {
        closedir(dir);
        arena_free(pkg_dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Check if it looks like a version (starts with digit) */
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            /* Verify it's a directory */
            size_t entry_len = strlen(pkg_dir) + strlen(entry->d_name) + 2;
            char *entry_path = arena_malloc(entry_len);
            if (entry_path) {
                snprintf(entry_path, entry_len, "%s/%s", pkg_dir, entry->d_name);
                struct stat entry_st;
                if (stat(entry_path, &entry_st) == 0 && S_ISDIR(entry_st.st_mode)) {
                    if (*count >= capacity) {
                        capacity *= 2;
                        char **new_versions = arena_realloc(versions, sizeof(char*) * capacity);
                        if (!new_versions) {
                            arena_free(entry_path);
                            break;
                        }
                        versions = new_versions;
                    }
                    versions[*count] = arena_strdup(entry->d_name);
                    if (versions[*count]) {
                        (*count)++;
                    }
                }
                arena_free(entry_path);
            }
        }
    }

    closedir(dir);
    arena_free(pkg_dir);
    return versions;
}

/* Print usage for cache check command */
static void print_cache_check_usage(void) {
    printf("Usage: %s package cache check <PACKAGE> [OPTIONS]\n", program_name);
    printf("\n");
    printf("Check cache status for a specific package.\n");
    printf("\n");
    printf("This command will:\n");
    printf("  - Search registry.dat and list known versions\n");
    printf("  - Search ELM_HOME for cached versions with valid src/ directory\n");
    printf("  - Report broken packages (missing or empty src/ directory)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package cache check elm/json            # Check cache status for elm/json\n", program_name);
    printf("  %s package cache check elm/html --purge-broken  # Remove broken cached versions\n", program_name);
    printf("  %s package cache check elm/html --fix-broken    # Re-download broken versions\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --purge-broken            Remove broken directories without re-downloading\n");
    printf("  --fix-broken              Try to re-download broken versions from registry\n");
    printf("  -v, --verbose             Show detailed output\n");
    printf("  --help                    Show this help\n");
}

int cache_check_package(const char *package_name, bool purge_broken, bool fix_broken, bool verbose) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    /* Initialize environment */
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        arena_free(author);
        arena_free(name);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    printf("\n%s-- CACHE CHECK: %s/%s --%s\n\n", ANSI_CYAN, author, name, ANSI_RESET);
    printf("ELM_HOME: %s\n", env->cache->elm_home);
    printf("Registry: %s\n\n", env->cache->registry_path);

    /* Find package in registry */
    RegistryEntry *entry = registry_find(env->registry, author, name);
    if (!entry) {
        printf("%sPackage not found in registry%s\n\n", ANSI_YELLOW, ANSI_RESET);
        printf("Note: The package might exist but is not in the cached registry.\n");
        printf("Try running 'elm-wrap install' to update the registry.\n");
    } else {
        printf("Registry versions (%zu):\n", entry->version_count);
        for (size_t i = 0; i < entry->version_count; i++) {
            char *ver_str = version_to_string(&entry->versions[i]);
            if (ver_str) {
                printf("  %s\n", ver_str);
                arena_free(ver_str);
            }
        }
        printf("\n");
    }

    /* List cached versions */
    size_t cached_count = 0;
    char **cached_versions = list_cached_versions(env->cache, author, name, &cached_count);

    if (cached_count == 0) {
        printf("Cached versions: %s(none)%s\n\n", ANSI_YELLOW, ANSI_RESET);
    } else {
        printf("Cached versions (%zu):\n", cached_count);

        size_t broken_count = 0;
        char **broken_versions = arena_malloc(sizeof(char*) * cached_count);
        if (!broken_versions) {
            install_env_free(env);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        for (size_t i = 0; i < cached_count; i++) {
            PackageStatus status = get_package_status(env->cache, author, name, cached_versions[i]);

            if (status == PKG_STATUS_OK) {
                printf("  %s%s%s %sOK%s\n", ANSI_CYAN, cached_versions[i], ANSI_RESET, ANSI_GREEN, ANSI_RESET);
            } else if (status == PKG_STATUS_BROKEN) {
                printf("  %s%s%s %sBROKEN%s (missing or empty src/)\n", 
                       ANSI_CYAN, cached_versions[i], ANSI_RESET, ANSI_RED, ANSI_RESET);
                broken_versions[broken_count++] = cached_versions[i];
            }
        }
        printf("\n");

        /* Handle --purge-broken */
        if (purge_broken && broken_count > 0) {
            printf("Purging %zu broken version(s):\n", broken_count);
            for (size_t i = 0; i < broken_count; i++) {
                char *pkg_path = cache_get_package_path(env->cache, author, name, broken_versions[i]);
                if (pkg_path) {
                    if (remove_directory_recursive(pkg_path)) {
                        printf("  %sRemoved%s %s\n", ANSI_GREEN, ANSI_RESET, broken_versions[i]);
                    } else {
                        printf("  %sFailed to remove%s %s\n", ANSI_RED, ANSI_RESET, broken_versions[i]);
                    }
                    arena_free(pkg_path);
                }
            }
            printf("\n");
        }

        /* Handle --fix-broken */
        if (fix_broken && broken_count > 0) {
            printf("Fixing %zu broken version(s):\n", broken_count);
            for (size_t i = 0; i < broken_count; i++) {
                char *pkg_path = cache_get_package_path(env->cache, author, name, broken_versions[i]);
                if (pkg_path) {
                    /* Remove existing broken directory first */
                    if (remove_directory_recursive(pkg_path)) {
                        if (verbose) {
                            printf("  Removed broken directory: %s\n", pkg_path);
                        }
                    }
                    arena_free(pkg_path);
                }

                printf("  Downloading %s/%s@%s... ", author, name, broken_versions[i]);
                fflush(stdout);

                if (install_env_download_package(env, author, name, broken_versions[i])) {
                    printf("%sOK%s\n", ANSI_GREEN, ANSI_RESET);
                } else {
                    printf("%sFAILED%s\n", ANSI_RED, ANSI_RESET);
                }
            }
            printf("\n");
        }

        arena_free(broken_versions);
    }

    /* Cleanup cached version strings */
    if (cached_versions) {
        for (size_t i = 0; i < cached_count; i++) {
            if (cached_versions[i]) {
                arena_free(cached_versions[i]);
            }
        }
        arena_free(cached_versions);
    }

    install_env_free(env);
    arena_free(author);
    arena_free(name);

    return 0;
}

int cmd_cache_check(int argc, char *argv[]) {
    const char *package_arg = NULL;
    bool purge_broken = false;
    bool fix_broken = false;
    bool verbose = false;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_cache_check_usage();
            return 0;
        } else if (strcmp(argv[i], "--purge-broken") == 0) {
            purge_broken = true;
        } else if (strcmp(argv[i], "--fix-broken") == 0) {
            fix_broken = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (argv[i][0] != '-') {
            if (package_arg) {
                fprintf(stderr, "Error: Multiple package names specified\n");
                return 1;
            }
            package_arg = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* Validate arguments */
    if (!package_arg) {
        fprintf(stderr, "Error: Package name is required\n");
        fprintf(stderr, "Usage: %s package cache check <PACKAGE>\n", program_name);
        return 1;
    }

    if (purge_broken && fix_broken) {
        fprintf(stderr, "Error: Cannot use both --purge-broken and --fix-broken\n");
        return 1;
    }

    return cache_check_package(package_arg, purge_broken, fix_broken, verbose);
}
