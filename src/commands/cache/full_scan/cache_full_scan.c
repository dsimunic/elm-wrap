#include "cache_full_scan.h"
#include "../../../cache.h"
#include "../../../global_context.h"
#include "../../../registry.h"
#include "../../../install_env.h"
#include "../../../alloc.h"
#include "../../../shared/log.h"
#include "../../../fileutil.h"
#include "../../../import_tree.h"
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
} PackageStatus;

/* Scan statistics */
typedef struct {
    size_t total_packages;
    size_t total_versions;
    size_t good_versions;
    size_t broken_versions;
    size_t packages_missing_latest;
    size_t packages_with_redundant;
    size_t total_redundant_files;
} ScanStats;

/* Check if a directory is empty */
static bool is_directory_empty(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return true;

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
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
static PackageStatus get_version_status(const char *version_path) {
    struct stat st;
    
    /* Check if src/ directory exists and has content */
    size_t src_len = strlen(version_path) + strlen("/src") + 1;
    char *src_path = arena_malloc(src_len);
    if (!src_path) {
        return PKG_STATUS_BROKEN;
    }

    snprintf(src_path, src_len, "%s/src", version_path);

    PackageStatus status;
    if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        status = PKG_STATUS_BROKEN;
    } else if (is_directory_empty(src_path)) {
        status = PKG_STATUS_BROKEN;
    } else {
        status = PKG_STATUS_OK;
    }

    arena_free(src_path);
    return status;
}

/* Check if the package has the latest version cached */
static bool has_latest_version_cached(Registry *registry, const char *author, 
                                       const char *name, const char *pkg_dir) {
    /* Find package in registry */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (!entry || entry->version_count == 0) {
        /* Not in registry, can't determine */
        return true;
    }
    
    /* Get latest version from registry */
    char *latest = version_to_string(&entry->versions[0]);
    if (!latest) {
        return true;
    }
    
    /* Check if this version directory exists */
    size_t path_len = strlen(pkg_dir) + strlen(latest) + 2;
    char *version_path = arena_malloc(path_len);
    if (!version_path) {
        arena_free(latest);
        return true;
    }
    
    snprintf(version_path, path_len, "%s/%s", pkg_dir, latest);
    
    struct stat st;
    bool exists = (stat(version_path, &st) == 0 && S_ISDIR(st.st_mode));
    
    /* Also check if it's a valid (non-broken) version */
    bool is_valid = exists && get_version_status(version_path) == PKG_STATUS_OK;
    
    arena_free(version_path);
    arena_free(latest);
    
    return is_valid;
}

/* Scan a single package directory */
static void scan_package(const char *packages_dir, const char *author, const char *name,
                         Registry *registry, ScanStats *stats, bool quiet, bool verbose,
                         bool check_redundant) {
    /* Build path to package directory */
    size_t pkg_dir_len = strlen(packages_dir) + strlen(author) + strlen(name) + 3;
    char *pkg_dir = arena_malloc(pkg_dir_len);
    if (!pkg_dir) return;
    
    snprintf(pkg_dir, pkg_dir_len, "%s/%s/%s", packages_dir, author, name);
    
    struct stat pkg_st;
    if (stat(pkg_dir, &pkg_st) != 0 || !S_ISDIR(pkg_st.st_mode)) {
        arena_free(pkg_dir);
        return;
    }
    
    stats->total_packages++;
    
    size_t pkg_broken = 0;
    char **broken_versions = NULL;
    size_t broken_capacity = 0;
    
    /* Find package in registry to iterate versions in sorted order */
    RegistryEntry *entry = registry_find(registry, author, name);
    if (entry && entry->version_count > 0) {
        /* Iterate registry versions (already sorted newest first) */
        for (size_t i = 0; i < entry->version_count; i++) {
            char *ver_str = version_to_string(&entry->versions[i]);
            if (!ver_str) continue;
            
            size_t version_path_len = strlen(pkg_dir) + strlen(ver_str) + 2;
            char *version_path = arena_malloc(version_path_len);
            if (!version_path) {
                arena_free(ver_str);
                continue;
            }
            
            snprintf(version_path, version_path_len, "%s/%s", pkg_dir, ver_str);
            
            struct stat st;
            if (stat(version_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                stats->total_versions++;
                
                PackageStatus status = get_version_status(version_path);
                if (status == PKG_STATUS_OK) {
                    stats->good_versions++;
                } else {
                    pkg_broken++;
                    stats->broken_versions++;
                    
                    /* Store broken version for reporting */
                    if (!quiet) {
                        if (pkg_broken > broken_capacity) {
                            size_t new_cap = broken_capacity == 0 ? 4 : broken_capacity * 2;
                            char **new_arr = arena_realloc(broken_versions, sizeof(char*) * new_cap);
                            if (new_arr) {
                                broken_versions = new_arr;
                                broken_capacity = new_cap;
                            }
                        }
                        if (pkg_broken <= broken_capacity) {
                            broken_versions[pkg_broken - 1] = ver_str;
                            ver_str = NULL; /* Don't free, stored in array */
                        }
                    }
                }
            }
            
            arena_free(version_path);
            if (ver_str) arena_free(ver_str);
        }
    }
    
    /* Check if latest version is cached */
    if (!has_latest_version_cached(registry, author, name, pkg_dir)) {
        stats->packages_missing_latest++;
        if (!quiet && verbose) {
            printf("%s%s/%s%s: missing latest version\n", ANSI_YELLOW, author, name, ANSI_RESET);
        }
    }
    
    /* Report broken versions if any */
    if (pkg_broken > 0 && !quiet) {
        printf("%s%s/%s%s: %zu broken version(s)\n", ANSI_RED, author, name, ANSI_RESET, pkg_broken);
        for (size_t i = 0; i < pkg_broken && broken_versions && broken_versions[i]; i++) {
            printf("  %s%s%s BROKEN\n", ANSI_CYAN, broken_versions[i], ANSI_RESET);
        }
    }
    
    /* Check for redundant files if requested */
    if (check_redundant && entry && entry->version_count > 0) {
        bool pkg_has_redundant = false;
        
        /* Iterate all registry versions and check those that are cached */
        for (size_t vi = 0; vi < entry->version_count; vi++) {
            char *ver_str = version_to_string(&entry->versions[vi]);
            if (!ver_str) continue;
            
            size_t version_path_len = strlen(pkg_dir) + strlen(ver_str) + 2;
            char *version_path = arena_malloc(version_path_len);
            if (!version_path) {
                arena_free(ver_str);
                continue;
            }
            
            snprintf(version_path, version_path_len, "%s/%s", pkg_dir, ver_str);
            
            /* Check if version is cached */
            struct stat st;
            if (stat(version_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                ImportTreeAnalysis *analysis = import_tree_analyze(version_path);
                if (analysis) {
                    int redundant = import_tree_redundant_count(analysis);
                    if (redundant > 0) {
                        if (!pkg_has_redundant) {
                            stats->packages_with_redundant++;
                            pkg_has_redundant = true;
                        }
                        stats->total_redundant_files += redundant;
                        if (!quiet) {
                            printf("%s%s/%s %s%s: %s%d redundant file(s)%s\n",
                                   ANSI_CYAN, author, name, ver_str, ANSI_RESET,
                                   ANSI_YELLOW, redundant, ANSI_RESET);
                            if (verbose) {
                                for (int ri = 0; ri < analysis->redundant_count; ri++) {
                                    printf("  • %s\n", analysis->redundant_files[ri]);
                                }
                            }
                        }
                    }
                    import_tree_free(analysis);
                }
            }
            
            arena_free(version_path);
            arena_free(ver_str);
        }
    }
    
    /* Free broken version strings */
    if (broken_versions) {
        for (size_t i = 0; i < pkg_broken && broken_versions[i]; i++) {
            arena_free(broken_versions[i]);
        }
        arena_free(broken_versions);
    }
    
    arena_free(pkg_dir);
}

/* Scan all packages under an author directory */
static void scan_author(const char *packages_dir, const char *author,
                        Registry *registry, ScanStats *stats, bool quiet, bool verbose,
                        bool check_redundant) {
    size_t author_dir_len = strlen(packages_dir) + strlen(author) + 2;
    char *author_dir = arena_malloc(author_dir_len);
    if (!author_dir) return;
    
    snprintf(author_dir, author_dir_len, "%s/%s", packages_dir, author);
    
    DIR *dir = opendir(author_dir);
    if (!dir) {
        arena_free(author_dir);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Skip registry.dat and other files */
        size_t path_len = strlen(author_dir) + strlen(entry->d_name) + 2;
        char *path = arena_malloc(path_len);
        if (!path) continue;
        
        snprintf(path, path_len, "%s/%s", author_dir, entry->d_name);
        
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            scan_package(packages_dir, author, entry->d_name, registry, stats, quiet, verbose,
                        check_redundant);
        }
        
        arena_free(path);
    }
    
    closedir(dir);
    arena_free(author_dir);
}

/* Print usage for cache full-scan command */
static void print_full_scan_usage(void) {
    printf("Usage: %s package cache full-scan [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Scan the entire package cache and verify all packages.\n");
    printf("\n");
    printf("This command will:\n");
    printf("  - Scan all packages in the cache\n");
    printf("  - Report broken packages (missing or empty src/ directory)\n");
    printf("  - Count packages missing the latest version\n");
    printf("  - Provide a summary of cache health\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package cache full-scan           # Scan and report broken packages\n", global_context_program_name());
    printf("  %s package cache full-scan -q        # Quiet mode - only show summary\n", global_context_program_name());
    printf("  %s package cache full-scan -v        # Verbose - show all issues\n", global_context_program_name());
    printf("  %s package cache full-scan --no-check-redundant  # Skip redundant file check\n", global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  -q, --quiet               Only show summary counts\n");
    printf("  -v, --verbose             Show all issues including missing latest\n");
    printf("  --no-check-redundant      Skip analyzing import trees for unused files\n");
    printf("  --help                Show this help\n");
}

int cmd_cache_full_scan(int argc, char *argv[]) {
    bool quiet = false;
    bool verbose = false;
    bool check_redundant = true;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_full_scan_usage();
            return 0;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--no-check-redundant") == 0) {
            check_redundant = false;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 1;
        }
    }
    
    /* Initialize environment */
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        return 1;
    }
    
    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        return 1;
    }
    
    if (!quiet) {
        printf("\n%s-- CACHE FULL SCAN --%s\n\n", ANSI_CYAN, ANSI_RESET);
        printf("Scanning: %s\n\n", env->cache->packages_dir);
    }
    
    ScanStats stats = {0};
    
    /* Open packages directory */
    DIR *dir = opendir(env->cache->packages_dir);
    if (!dir) {
        fprintf(stderr, "Error: Cannot open packages directory: %s\n", env->cache->packages_dir);
        install_env_free(env);
        return 1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Skip registry.dat */
        if (strcmp(entry->d_name, "registry.dat") == 0) {
            continue;
        }
        
        /* Check if it's a directory (author) */
        size_t path_len = strlen(env->cache->packages_dir) + strlen(entry->d_name) + 2;
        char *path = arena_malloc(path_len);
        if (!path) continue;
        
        snprintf(path, path_len, "%s/%s", env->cache->packages_dir, entry->d_name);
        
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            scan_author(env->cache->packages_dir, entry->d_name, env->registry, &stats, quiet, verbose,
                       check_redundant);
        }
        
        arena_free(path);
    }
    
    closedir(dir);
    
    /* Print summary */
    if (!quiet) {
        printf("\n%s-- SUMMARY --%s\n", ANSI_CYAN, ANSI_RESET);
    }
    
    printf("Packages scanned:       %zu\n", stats.total_packages);
    printf("Total versions:         %zu\n", stats.total_versions);
    printf("  %sGood versions:%s        %zu\n", ANSI_GREEN, ANSI_RESET, stats.good_versions);
    
    if (stats.broken_versions > 0) {
        printf("  %sBroken versions:%s      %zu\n", ANSI_RED, ANSI_RESET, stats.broken_versions);
    } else {
        printf("  Broken versions:      0\n");
    }
    
    if (stats.packages_missing_latest > 0) {
        printf("%sMissing latest version:%s %zu package(s)\n", ANSI_YELLOW, ANSI_RESET, stats.packages_missing_latest);
    }
    
    if (check_redundant) {
        if (stats.packages_with_redundant > 0) {
            printf("%sPackages with redundant files:%s %zu (%zu total files)\n", 
                   ANSI_YELLOW, ANSI_RESET, stats.packages_with_redundant, stats.total_redundant_files);
        } else {
            printf("Packages with redundant files: 0\n");
        }
    }
    
    install_env_free(env);
    
    return 0;
}
