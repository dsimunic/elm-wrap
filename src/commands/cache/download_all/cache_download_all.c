#include "cache_download_all.h"
#include "../cache_common.h"
#include "../../../cache.h"
#include "../../../global_context.h"
#include "../../../registry.h"
#include "../../../install_env.h"
#include "../../../alloc.h"
#include "../../../shared/log.h"
#include "../../../fileutil.h"
#include "../../../constants.h"
#include "../../../http_client.h"
#include "../../../env_defaults.h"
#include "../../package/package_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

/* ANSI color codes */
#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_RESET  "\033[0m"

/* Retry and delay settings */
#define MAX_RETRIES 3
#define INITIAL_BACKOFF_SECS 2
#define MIN_DELAY_SECS 1
#define MAX_DELAY_SECS 15

/* Blacklist file name (relative to WRAP_HOME) */
#define BLACKLIST_FILENAME "blacklist.txt"

/* Random delay between MIN_DELAY_SECS and MAX_DELAY_SECS */
static void random_delay(void) {
    int delay = MIN_DELAY_SECS + (rand() % (MAX_DELAY_SECS - MIN_DELAY_SECS + 1));
    sleep((unsigned int)delay);
}

/* Blacklist entry - can match all versions or specific version */
typedef struct {
    char *author;
    char *name;
    char *version;  /* NULL means all versions */
} BlacklistEntry;

/* Blacklist container */
typedef struct {
    BlacklistEntry *entries;
    size_t count;
    size_t capacity;
} Blacklist;

/* Create empty blacklist */
static Blacklist* blacklist_create(void) {
    Blacklist *bl = arena_malloc(sizeof(Blacklist));
    if (!bl) return NULL;

    bl->capacity = 256;
    bl->count = 0;
    bl->entries = arena_malloc(sizeof(BlacklistEntry) * bl->capacity);
    if (!bl->entries) {
        arena_free(bl);
        return NULL;
    }
    return bl;
}

/* Free blacklist */
static void blacklist_free(Blacklist *bl) {
    if (!bl) return;
    for (size_t i = 0; i < bl->count; i++) {
        if (bl->entries[i].author) arena_free(bl->entries[i].author);
        if (bl->entries[i].name) arena_free(bl->entries[i].name);
        if (bl->entries[i].version) arena_free(bl->entries[i].version);
    }
    arena_free(bl->entries);
    arena_free(bl);
}

/* Add entry to blacklist */
static bool blacklist_add(Blacklist *bl, const char *author, const char *name, const char *version) {
    if (!bl) return false;

    if (bl->count >= bl->capacity) {
        size_t new_cap = bl->capacity * 2;
        BlacklistEntry *new_entries = arena_realloc(bl->entries, sizeof(BlacklistEntry) * new_cap);
        if (!new_entries) return false;
        bl->entries = new_entries;
        bl->capacity = new_cap;
    }

    BlacklistEntry *entry = &bl->entries[bl->count];
    entry->author = arena_strdup(author);
    entry->name = arena_strdup(name);
    entry->version = version ? arena_strdup(version) : NULL;

    if (!entry->author || !entry->name || (version && !entry->version)) {
        if (entry->author) arena_free(entry->author);
        if (entry->name) arena_free(entry->name);
        if (entry->version) arena_free(entry->version);
        return false;
    }

    bl->count++;
    return true;
}

/* Check if package/version is blacklisted */
static bool blacklist_contains(Blacklist *bl, const char *author, const char *name, const char *version) {
    if (!bl) return false;

    for (size_t i = 0; i < bl->count; i++) {
        BlacklistEntry *entry = &bl->entries[i];

        if (strcmp(entry->author, author) == 0 && strcmp(entry->name, name) == 0) {
            /* Match: either no version specified (all versions) or exact version match */
            if (!entry->version || strcmp(entry->version, version) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Load blacklist from WRAP_HOME/blacklist.txt */
static Blacklist* blacklist_load(bool verbose) {
    Blacklist *bl = blacklist_create();
    if (!bl) return NULL;

    char *wrap_home = env_get_wrap_home();
    if (!wrap_home) {
        return bl;  /* Return empty blacklist if no WRAP_HOME */
    }

    size_t path_len = strlen(wrap_home) + 1 + strlen(BLACKLIST_FILENAME) + 1;
    char *blacklist_path = arena_malloc(path_len);
    if (!blacklist_path) {
        arena_free(wrap_home);
        return bl;
    }
    snprintf(blacklist_path, path_len, "%s/%s", wrap_home, BLACKLIST_FILENAME);
    arena_free(wrap_home);

    FILE *f = fopen(blacklist_path, "r");
    if (!f) {
        if (verbose) {
            printf("No blacklist file found at %s\n", blacklist_path);
        }
        arena_free(blacklist_path);
        return bl;  /* Return empty blacklist if file doesn't exist */
    }

    if (verbose) {
        printf("Loading blacklist from %s\n", blacklist_path);
    }

    char line[512];
    size_t line_num = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;

        /* Trim trailing newline/whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                          line[len-1] == ' ' || line[len-1] == '\t')) {
            line[--len] = '\0';
        }

        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#') {
            continue;
        }

        /* Strip inline comments (anything after # or double-space) */
        char *comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
            /* Trim trailing whitespace again */
            len = strlen(line);
            while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) {
                line[--len] = '\0';
            }
        }

        /* Also strip at double-space (our fail-log format) */
        char *dblspace = strstr(line, "  ");
        if (dblspace) {
            *dblspace = '\0';
            len = strlen(line);
        }

        if (len == 0) {
            continue;
        }

        /* Parse author/name[@version] */
        char *slash = strchr(line, '/');
        if (!slash) {
            fprintf(stderr, "Warning: Invalid blacklist entry at line %zu: %s\n", line_num, line);
            continue;
        }

        *slash = '\0';
        char *author = line;
        char *name_and_version = slash + 1;

        char *at_sign = strchr(name_and_version, '@');
        char *name;
        char *version = NULL;

        if (at_sign) {
            *at_sign = '\0';
            name = name_and_version;
            version = at_sign + 1;
        } else {
            name = name_and_version;
        }

        if (strlen(author) == 0 || strlen(name) == 0) {
            fprintf(stderr, "Warning: Invalid blacklist entry at line %zu\n", line_num);
            continue;
        }

        blacklist_add(bl, author, name, version);
    }

    fclose(f);
    arena_free(blacklist_path);

    if (verbose) {
        printf("Loaded %zu blacklist entries\n", bl->count);
    }

    return bl;
}

/* Status of a cached package version */
typedef enum {
    VERSION_OK,           /* Valid: has src/ with content */
    VERSION_BROKEN,       /* Exists but missing/empty src/ */
    VERSION_NOT_CACHED    /* Not in cache at all */
} VersionStatus;

/* A single version to download */
typedef struct {
    char *author;
    char *name;
    char *version;
    VersionStatus status;  /* Whether it's broken (needs removal) or missing */
} DownloadItem;

/* Queue of packages to download */
typedef struct {
    DownloadItem *items;
    size_t count;
    size_t capacity;
} DownloadQueue;

/* Statistics */
typedef struct {
    size_t total_packages;
    size_t total_versions;
    size_t already_cached;
    size_t broken;
    size_t missing;
    size_t skipped;         /* Blacklisted packages */
    size_t downloaded_ok;
    size_t download_failed;
} DownloadStats;

/* Check if directory is empty */
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

/* Get status of a specific package version */
static VersionStatus get_version_status(CacheConfig *cache,
                                         const char *author,
                                         const char *name,
                                         const char *version) {
    char *pkg_path = cache_get_package_path(cache, author, name, version);
    if (!pkg_path) return VERSION_NOT_CACHED;

    struct stat st;
    if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        arena_free(pkg_path);
        return VERSION_NOT_CACHED;
    }

    /* Check src/ directory */
    size_t src_len = strlen(pkg_path) + strlen("/src") + 1;
    char *src_path = arena_malloc(src_len);
    if (!src_path) {
        arena_free(pkg_path);
        return VERSION_NOT_CACHED;
    }

    snprintf(src_path, src_len, "%s/src", pkg_path);

    VersionStatus status;
    if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        status = VERSION_BROKEN;
    } else if (is_directory_empty(src_path)) {
        status = VERSION_BROKEN;
    } else {
        status = VERSION_OK;
    }

    arena_free(src_path);
    arena_free(pkg_path);
    return status;
}

/* Initialize download queue */
static DownloadQueue* queue_create(void) {
    DownloadQueue *q = arena_malloc(sizeof(DownloadQueue));
    if (!q) return NULL;

    q->capacity = INITIAL_SMALL_CAPACITY;
    q->count = 0;
    q->items = arena_malloc(sizeof(DownloadItem) * q->capacity);
    if (!q->items) {
        arena_free(q);
        return NULL;
    }
    return q;
}

/* Add item to download queue */
static bool queue_add(DownloadQueue *q, const char *author, const char *name,
                      const char *version, VersionStatus status) {
    if (!q) return false;

    if (q->count >= q->capacity) {
        size_t new_cap = q->capacity * 2;
        DownloadItem *new_items = arena_realloc(q->items, sizeof(DownloadItem) * new_cap);
        if (!new_items) return false;
        q->items = new_items;
        q->capacity = new_cap;
    }

    DownloadItem *item = &q->items[q->count];
    item->author = arena_strdup(author);
    item->name = arena_strdup(name);
    item->version = arena_strdup(version);
    item->status = status;

    if (!item->author || !item->name || !item->version) {
        if (item->author) arena_free(item->author);
        if (item->name) arena_free(item->name);
        if (item->version) arena_free(item->version);
        return false;
    }

    q->count++;
    return true;
}

/* Free download queue */
static void queue_free(DownloadQueue *q) {
    if (!q) return;
    for (size_t i = 0; i < q->count; i++) {
        if (q->items[i].author) arena_free(q->items[i].author);
        if (q->items[i].name) arena_free(q->items[i].name);
        if (q->items[i].version) arena_free(q->items[i].version);
    }
    arena_free(q->items);
    arena_free(q);
}

/* Print usage */
static void print_usage(void) {
    const char *prog = global_context_program_name();
    printf("Usage: %s package cache download-all [OPTIONS]\n", prog);
    printf("\n");
    printf("Download all packages from the Elm registry to the local cache.\n");
    printf("\n");
    printf("Packages are downloaded directly from GitHub archives.\n");
    printf("\n");
    printf("This command will:\n");
    printf("  - Update registry.dat with latest package information\n");
    printf("  - Check each package version listed in the registry\n");
    printf("  - Skip packages listed in WRAP_HOME/blacklist.txt\n");
    printf("  - Download missing/broken packages from GitHub\n");
    printf("  - Fix broken packages (missing/empty src/) by re-downloading\n");
    printf("\n");
    printf("Blacklist format (one entry per line):\n");
    printf("  author/name           # Skip all versions\n");
    printf("  author/name@1.0.0     # Skip specific version\n");
    printf("  # Lines starting with # are comments\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes         Skip confirmation prompt\n");
    printf("  -q, --quiet       Only show summary\n");
    printf("  -v, --verbose     Show detailed progress for each package\n");
    printf("  --dry-run         Show what would be downloaded without downloading\n");
    printf("  --latest-only     Only download the latest version of each package\n");
    printf("  --fail-log FILE   Write failed packages to FILE in blacklist format\n");
    printf("  --help            Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package cache download-all              # Download all packages\n", prog);
    printf("  %s package cache download-all --dry-run    # Preview what would download\n", prog);
    printf("  %s package cache download-all --latest-only # Only latest versions\n", prog);
}

int cmd_cache_download_all(int argc, char *argv[]) {
    bool confirm = true;
    bool quiet = false;
    bool verbose = false;
    bool dry_run = false;
    bool latest_only = false;
    const char *fail_log_path = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            confirm = false;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--latest-only") == 0) {
            latest_only = true;
        } else if (strcmp(argv[i], "--fail-log") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --fail-log requires a file path\n");
                return 1;
            }
            fail_log_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* Initialize environment (updates registry.dat) */
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

    /* Set longer timeout for bulk downloads (60 seconds) */
    curl_session_set_timeout(env->curl_session, 60000L);

    if (!quiet) {
        printf("\n%s-- CACHE DOWNLOAD-ALL --%s\n\n", ANSI_CYAN, ANSI_RESET);
        printf("Registry: %s\n", env->cache->registry_path);
        printf("Cache:    %s\n", env->cache->packages_dir);
        printf("Packages in registry: %zu\n\n", env->registry->entry_count);
    }

    /* Create download queue */
    DownloadQueue *queue = queue_create();
    if (!queue) {
        log_error("Failed to create download queue");
        install_env_free(env);
        return 1;
    }

    DownloadStats stats = {0};

    /* Load blacklist from WRAP_HOME/blacklist.txt */
    Blacklist *blacklist = blacklist_load(verbose);

    /* Open fail log file if specified */
    FILE *fail_log = NULL;
    if (fail_log_path) {
        fail_log = fopen(fail_log_path, "w");
        if (!fail_log) {
            fprintf(stderr, "Error: Cannot open fail log file: %s\n", fail_log_path);
            blacklist_free(blacklist);
            queue_free(queue);
            install_env_free(env);
            return 1;
        }
        fprintf(fail_log, "# Failed packages from download-all\n");
        fprintf(fail_log, "# Can be used as blacklist.txt\n\n");
        fflush(fail_log);
    }

    /* Phase 1: Scan registry and build download queue */
    if (!quiet) {
        printf("Scanning registry...\n");
    }

    for (size_t pkg_idx = 0; pkg_idx < env->registry->entry_count; pkg_idx++) {
        RegistryEntry *entry = &env->registry->entries[pkg_idx];
        stats.total_packages++;

        size_t versions_to_check = latest_only ? 1 : entry->version_count;

        for (size_t ver_idx = 0; ver_idx < versions_to_check && ver_idx < entry->version_count; ver_idx++) {
            char *ver_str = version_to_string(&entry->versions[ver_idx]);
            if (!ver_str) continue;

            stats.total_versions++;

            /* Check if blacklisted */
            if (blacklist_contains(blacklist, entry->author, entry->name, ver_str)) {
                stats.skipped++;
                if (verbose) {
                    printf("  %s/%s %s SKIPPED (blacklisted)\n",
                           entry->author, entry->name, ver_str);
                }
                arena_free(ver_str);
                continue;
            }

            VersionStatus status = get_version_status(env->cache,
                                                       entry->author,
                                                       entry->name,
                                                       ver_str);

            switch (status) {
                case VERSION_OK:
                    stats.already_cached++;
                    if (verbose) {
                        printf("  %s%s/%s %s%s OK\n",
                               ANSI_GREEN, entry->author, entry->name, ver_str, ANSI_RESET);
                    }
                    break;

                case VERSION_BROKEN:
                    stats.broken++;
                    queue_add(queue, entry->author, entry->name, ver_str, status);
                    if (verbose) {
                        printf("  %s%s/%s %s%s BROKEN (will fix)\n",
                               ANSI_RED, entry->author, entry->name, ver_str, ANSI_RESET);
                    }
                    break;

                case VERSION_NOT_CACHED:
                    stats.missing++;
                    queue_add(queue, entry->author, entry->name, ver_str, status);
                    if (verbose) {
                        printf("  %s%s/%s %s%s MISSING\n",
                               ANSI_YELLOW, entry->author, entry->name, ver_str, ANSI_RESET);
                    }
                    break;
            }

            arena_free(ver_str);
        }
    }

    /* Report scan results - always printed */
    printf("\n%s-- SCAN COMPLETE --%s\n", ANSI_CYAN, ANSI_RESET);
    printf("Total packages:    %zu\n", stats.total_packages);
    printf("Total versions:    %zu\n", stats.total_versions);
    printf("Already cached:    %s%zu%s\n", ANSI_GREEN, stats.already_cached, ANSI_RESET);
    printf("Broken (to fix):   %s%zu%s\n", stats.broken > 0 ? ANSI_RED : "",
           stats.broken, stats.broken > 0 ? ANSI_RESET : "");
    printf("Missing:           %s%zu%s\n", stats.missing > 0 ? ANSI_YELLOW : "",
           stats.missing, stats.missing > 0 ? ANSI_RESET : "");
    if (stats.skipped > 0) {
        printf("Skipped:           %zu (blacklisted)\n", stats.skipped);
    }
    printf("To download:       %zu\n", queue->count);

    /* Nothing to download? */
    if (queue->count == 0) {
        printf("\n%sAll packages are already cached and valid!%s\n", ANSI_GREEN, ANSI_RESET);
        if (fail_log) fclose(fail_log);
        blacklist_free(blacklist);
        queue_free(queue);
        install_env_free(env);
        return 0;
    }

    /* Dry run - show what would be downloaded */
    if (dry_run) {
        if (!quiet) {
            printf("\nWould download:\n");
            for (size_t i = 0; i < queue->count; i++) {
                DownloadItem *item = &queue->items[i];
                printf("  %s/%s %s%s\n", item->author, item->name, item->version,
                       item->status == VERSION_BROKEN ? " (fix broken)" : "");
            }
        }
        printf("\n");
        if (fail_log) fclose(fail_log);
        blacklist_free(blacklist);
        queue_free(queue);
        install_env_free(env);
        return 0;
    }

    /* Confirm with user */
    if (confirm) {
        printf("Download %zu package version(s)? [y/N] ", queue->count);
        fflush(stdout);

        char response[16];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'y' && response[0] != 'Y')) {
            printf("Aborted.\n");
            if (fail_log) fclose(fail_log);
            blacklist_free(blacklist);
            queue_free(queue);
            install_env_free(env);
            return 0;
        }
    }

    /* Phase 2: Download all queued packages */
    if (!quiet) {
        printf("\n%s-- DOWNLOADING --%s\n\n", ANSI_CYAN, ANSI_RESET);
    }

    /* Seed random number generator for delays */
    srand((unsigned int)time(NULL));

    for (size_t i = 0; i < queue->count; i++) {
        DownloadItem *item = &queue->items[i];

        if (!quiet) {
            printf("[%zu/%zu] %s/%s %s ", i + 1, queue->count,
                   item->author, item->name, item->version);

            if (item->status == VERSION_BROKEN) {
                printf("(fixing) ");
            }
            fflush(stdout);
        }

        /* If broken, remove existing directory first */
        if (item->status == VERSION_BROKEN) {
            char *pkg_path = cache_get_package_path(env->cache, item->author,
                                                     item->name, item->version);
            if (pkg_path) {
                remove_directory_recursive(pkg_path);
                arena_free(pkg_path);
            }
        }

        /* Download the package with retry logic */
        bool success = false;
        int backoff_secs = INITIAL_BACKOFF_SECS;
        char error_msg[256] = {0};

        for (int attempt = 0; attempt < MAX_RETRIES && !success; attempt++) {
            if (attempt > 0) {
                /* Retry: remove any partial download first */
                char *pkg_path = cache_get_package_path(env->cache, item->author,
                                                         item->name, item->version);
                if (pkg_path) {
                    remove_directory_recursive(pkg_path);
                    arena_free(pkg_path);
                }

                if (!quiet) {
                    printf("retry %d/%d after %ds... ", attempt, MAX_RETRIES - 1, backoff_secs);
                    fflush(stdout);
                }
                sleep((unsigned int)backoff_secs);
                backoff_secs *= 2;  /* Exponential backoff */
            }

            if (cache_download_from_github(env, item->author, item->name, item->version,
                                            verbose, error_msg, sizeof(error_msg))) {
                success = true;
            }
        }

        if (success) {
            stats.downloaded_ok++;
            if (!quiet) {
                printf("%sOK%s\n", ANSI_GREEN, ANSI_RESET);
            }
        } else {
            stats.download_failed++;
            if (!quiet) {
                printf("%sFAILED%s (%s)\n", ANSI_RED, ANSI_RESET,
                       error_msg[0] ? error_msg : "unknown error");
            }
            /* Write to fail log immediately */
            if (fail_log) {
                fprintf(fail_log, "%s/%s@%s  # %s\n",
                        item->author, item->name, item->version,
                        error_msg[0] ? error_msg : "unknown error");
                fflush(fail_log);
            }
        }

        /* Random delay between requests to be gentle on the server */
        if (i < queue->count - 1) {
            random_delay();
        }
    }

    /* Final summary - always printed */
    printf("\n%s-- SUMMARY --%s\n", ANSI_CYAN, ANSI_RESET);
    printf("Downloaded:        %s%zu%s\n", ANSI_GREEN, stats.downloaded_ok, ANSI_RESET);
    if (stats.download_failed > 0) {
        printf("Failed:            %s%zu%s\n", ANSI_RED, stats.download_failed, ANSI_RESET);
        if (fail_log) {
            printf("Failures logged to: %s\n", fail_log_path);
        }
    }

    if (fail_log) fclose(fail_log);
    blacklist_free(blacklist);
    queue_free(queue);
    install_env_free(env);

    return stats.download_failed > 0 ? 1 : 0;
}
