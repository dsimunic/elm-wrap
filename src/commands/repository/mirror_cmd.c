#include "mirror_cmd.h"
#include "../package/package_common.h"
#include "../../mirror_manifest.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../cache.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../fileutil.h"
#include "../../http_client.h"
#include "../../env_defaults.h"
#include "../../global_context.h"
#include "../../shared/log.h"
#include "../../protocol_v1/package_fetch.h"
#include "../../vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

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

/* State file names (relative to WRAP_HOME) */
#define BLACKLIST_FILENAME "blacklist.txt"
#define MIRROR_SINCE_FILENAME "mirror-since.txt"

/* Default output paths */
#define DEFAULT_OUTPUT_DIR "./mirror"
#define DEFAULT_MANIFEST_FILE "manifest.json"

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

/* A single version to mirror */
typedef struct {
    char *author;
    char *name;
    char *version;
    size_t seq;     /* Sequence number for ordering */
} MirrorItem;

/* Queue of packages to mirror */
typedef struct {
    MirrorItem *items;
    size_t count;
    size_t capacity;
} MirrorQueue;

/* Statistics */
typedef struct {
    size_t total_packages;
    size_t total_versions;
    size_t already_mirrored;
    size_t skipped;          /* Blacklisted packages */
    size_t downloaded_ok;
    size_t download_failed;
} MirrorStats;

/* Command options */
typedef struct {
    const char *output_dir;
    const char *manifest_path;
    const char *fail_log_path;
    bool full_sync;
    bool latest_only;
    bool dry_run;
    bool confirm;
    bool verbose;
    bool quiet;
    char **specific_packages;    /* --package author/name */
    size_t specific_package_count;
    size_t specific_package_capacity;
} MirrorOptions;

/* Random delay between MIN_DELAY_SECS and MAX_DELAY_SECS */
static void random_delay(void) {
    int delay = MIN_DELAY_SECS + (rand() % (MAX_DELAY_SECS - MIN_DELAY_SECS + 1));
    sleep((unsigned int)delay);
}

/* --- Blacklist functions --- */

static Blacklist* blacklist_create(void) {
    Blacklist *bl = arena_malloc(sizeof(Blacklist));
    if (!bl) return NULL;

    bl->capacity = INITIAL_LARGE_CAPACITY;
    bl->count = 0;
    bl->entries = arena_malloc(sizeof(BlacklistEntry) * bl->capacity);
    if (!bl->entries) {
        arena_free(bl);
        return NULL;
    }
    return bl;
}

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

static bool blacklist_contains(Blacklist *bl, const char *author, const char *name, const char *version) {
    if (!bl) return false;

    for (size_t i = 0; i < bl->count; i++) {
        BlacklistEntry *entry = &bl->entries[i];

        if (strcmp(entry->author, author) == 0 && strcmp(entry->name, name) == 0) {
            if (!entry->version || strcmp(entry->version, version) == 0) {
                return true;
            }
        }
    }
    return false;
}

static Blacklist* blacklist_load(bool verbose) {
    Blacklist *bl = blacklist_create();
    if (!bl) return NULL;

    char *wrap_home = env_get_wrap_home();
    if (!wrap_home) {
        return bl;
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
        return bl;
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

        /* Strip inline comments */
        char *comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
            len = strlen(line);
            while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) {
                line[--len] = '\0';
            }
        }

        /* Strip at double-space (fail-log format) */
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

/* --- Mirror since-count persistence --- */

static char* get_mirror_since_path(void) {
    char *wrap_home = env_get_wrap_home();
    if (!wrap_home) return NULL;

    size_t path_len = strlen(wrap_home) + 1 + strlen(MIRROR_SINCE_FILENAME) + 1;
    char *path = arena_malloc(path_len);
    if (!path) {
        arena_free(wrap_home);
        return NULL;
    }
    snprintf(path, path_len, "%s/%s", wrap_home, MIRROR_SINCE_FILENAME);
    arena_free(wrap_home);
    return path;
}

static bool mirror_since_read(size_t *out_since) {
    if (!out_since) return false;

    char *path = get_mirror_since_path();
    if (!path) return false;

    char *contents = file_read_contents_bounded(path, MAX_TEMP_BUFFER_LENGTH, NULL);
    arena_free(path);

    if (!contents) return false;

    /* Trim whitespace */
    size_t len = strlen(contents);
    while (len > 0 && (contents[len-1] == '\n' || contents[len-1] == '\r' ||
                       contents[len-1] == ' ' || contents[len-1] == '\t')) {
        contents[--len] = '\0';
    }

    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(contents, &end, 10);
    if (errno != 0 || !end || end == contents || *end != '\0') {
        arena_free(contents);
        return false;
    }

    if (val > (unsigned long long)SIZE_MAX) {
        arena_free(contents);
        return false;
    }

    *out_since = (size_t)val;
    arena_free(contents);
    return true;
}

static bool mirror_since_write(size_t since) {
    char *path = get_mirror_since_path();
    if (!path) return false;

    char buf[64];
    int written = snprintf(buf, sizeof(buf), "%zu\n", since);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        arena_free(path);
        return false;
    }

    bool ok = file_write_bytes_atomic(path, buf, strlen(buf));
    arena_free(path);
    return ok;
}

/* --- Mirror queue --- */

static MirrorQueue* queue_create(void) {
    MirrorQueue *q = arena_malloc(sizeof(MirrorQueue));
    if (!q) return NULL;

    q->capacity = INITIAL_SMALL_CAPACITY;
    q->count = 0;
    q->items = arena_malloc(sizeof(MirrorItem) * q->capacity);
    if (!q->items) {
        arena_free(q);
        return NULL;
    }
    return q;
}

static bool queue_add(MirrorQueue *q, const char *author, const char *name,
                      const char *version, size_t seq) {
    if (!q) return false;

    if (q->count >= q->capacity) {
        size_t new_cap = q->capacity * 2;
        MirrorItem *new_items = arena_realloc(q->items, sizeof(MirrorItem) * new_cap);
        if (!new_items) return false;
        q->items = new_items;
        q->capacity = new_cap;
    }

    MirrorItem *item = &q->items[q->count];
    item->author = arena_strdup(author);
    item->name = arena_strdup(name);
    item->version = arena_strdup(version);
    item->seq = seq;

    if (!item->author || !item->name || !item->version) {
        if (item->author) arena_free(item->author);
        if (item->name) arena_free(item->name);
        if (item->version) arena_free(item->version);
        return false;
    }

    q->count++;
    return true;
}

static void queue_free(MirrorQueue *q) {
    if (!q) return;
    for (size_t i = 0; i < q->count; i++) {
        if (q->items[i].author) arena_free(q->items[i].author);
        if (q->items[i].name) arena_free(q->items[i].name);
        if (q->items[i].version) arena_free(q->items[i].version);
    }
    arena_free(q->items);
    arena_free(q);
}

/* --- Helper: get current ISO 8601 timestamp --- */

static char* get_iso8601_timestamp(void) {
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    if (!tm_info) return NULL;

    char *buf = arena_malloc(32);
    if (!buf) return NULL;

    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", tm_info);
    return buf;
}

/* --- Helper: check if specific package is in the filter list --- */

static bool is_package_in_filter(MirrorOptions *opts, const char *author, const char *name) {
    if (opts->specific_package_count == 0) return true;  /* No filter = include all */

    for (size_t i = 0; i < opts->specific_package_count; i++) {
        char *slash = strchr(opts->specific_packages[i], '/');
        if (!slash) continue;

        size_t author_len = (size_t)(slash - opts->specific_packages[i]);
        const char *filter_name = slash + 1;

        if (strlen(author) == author_len &&
            strncmp(author, opts->specific_packages[i], author_len) == 0 &&
            strcmp(name, filter_name) == 0) {
            return true;
        }
    }
    return false;
}

/* --- Helper: store archive by hash --- */

static bool store_archive_by_hash(const char *temp_path, const char *hash,
                                   const char *archives_dir) {
    /* Build destination path: archives/{hash}.zip */
    size_t dest_len = strlen(archives_dir) + 1 + strlen(hash) + 4 + 1;  /* /hash.zip */
    char *dest_path = arena_malloc(dest_len);
    if (!dest_path) return false;

    snprintf(dest_path, dest_len, "%s/%s.zip", archives_dir, hash);

    /* Check if already exists (hash collision / dedup) */
    struct stat st;
    if (stat(dest_path, &st) == 0) {
        arena_free(dest_path);
        return true;  /* Already exists, no need to copy */
    }

    /* Copy temp file to destination */
    FILE *src = fopen(temp_path, "rb");
    if (!src) {
        arena_free(dest_path);
        return false;
    }

    FILE *dst = fopen(dest_path, "wb");
    if (!dst) {
        fclose(src);
        arena_free(dest_path);
        return false;
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            unlink(dest_path);
            arena_free(dest_path);
            return false;
        }
    }

    fclose(src);
    fclose(dst);
    arena_free(dest_path);
    return true;
}

/* --- Helper: store metadata files --- */

static bool store_metadata(InstallEnv *env, const char *author, const char *name,
                           const char *version, const char *packages_dir) {
    /* Build destination directory: packages/{author}/{name}/{version}/ */
    size_t dir_len = strlen(packages_dir) + 1 + strlen(author) + 1 +
                     strlen(name) + 1 + strlen(version) + 1;
    char *pkg_dir = arena_malloc(dir_len);
    if (!pkg_dir) return false;

    snprintf(pkg_dir, dir_len, "%s/%s/%s/%s", packages_dir, author, name, version);

    if (!mkdir_p(pkg_dir)) {
        arena_free(pkg_dir);
        return false;
    }

    /* Copy elm.json from cache */
    char *elm_json_src = build_package_file_path(env->cache->packages_dir,
                                                  author, name, version, "elm.json");
    if (elm_json_src) {
        size_t elm_json_dst_len = strlen(pkg_dir) + strlen("/elm.json") + 1;
        char *elm_json_dst = arena_malloc(elm_json_dst_len);
        if (elm_json_dst) {
            snprintf(elm_json_dst, elm_json_dst_len, "%s/elm.json", pkg_dir);

            char *content = file_read_contents_bounded(elm_json_src, MAX_ELM_JSON_FILE_BYTES, NULL);
            if (content) {
                file_write_bytes_atomic(elm_json_dst, content, strlen(content));
                arena_free(content);
            }
            arena_free(elm_json_dst);
        }
        arena_free(elm_json_src);
    }

    /* Copy docs.json from cache */
    char *docs_json_src = build_package_file_path(env->cache->packages_dir,
                                                   author, name, version, "docs.json");
    if (docs_json_src) {
        size_t docs_json_dst_len = strlen(pkg_dir) + strlen("/docs.json") + 1;
        char *docs_json_dst = arena_malloc(docs_json_dst_len);
        if (docs_json_dst) {
            snprintf(docs_json_dst, docs_json_dst_len, "%s/docs.json", pkg_dir);

            char *content = file_read_contents_bounded(docs_json_src, MAX_DOCS_JSON_FILE_BYTES, NULL);
            if (content) {
                file_write_bytes_atomic(docs_json_dst, content, strlen(content));
                arena_free(content);
            }
            arena_free(docs_json_dst);
        }
        arena_free(docs_json_src);
    }

    arena_free(pkg_dir);
    return true;
}

/* --- Print usage --- */

static void print_usage(void) {
    const char *prog = global_context_program_name();
    printf("Usage: %s repository mirror [OPTIONS] [OUTPUT_DIR]\n", prog);
    printf("\n");
    printf("Create a content-addressable mirror of Elm packages.\n");
    printf("\n");
    printf("This command creates a mirror suitable for self-hosted infrastructure:\n");
    printf("  - Archives stored by SHA1 hash for deduplication\n");
    printf("  - elm.json and docs.json metadata in packages/ directory\n");
    printf("  - manifest.json mapping packages to hashes\n");
    printf("  - Incremental sync using sequence numbers\n");
    printf("\n");
    printf("Options:\n");
    printf("  --output-dir PATH     Directory for output (default: ./mirror/)\n");
    printf("  --manifest PATH       Output manifest file (default: OUTPUT_DIR/manifest.json)\n");
    printf("  --full                Process entire registry (ignore last-processed marker)\n");
    printf("  --latest-only         Only mirror latest version of each package\n");
    printf("  --package AUTHOR/NAME Mirror specific package only (can repeat)\n");
    printf("  --dry-run             Show what would be downloaded\n");
    printf("  -y, --yes             Skip confirmation prompt\n");
    printf("  -v, --verbose         Show detailed progress\n");
    printf("  -q, --quiet           Only show summary\n");
    printf("  --fail-log PATH       Write failures in blacklist format\n");
    printf("  --help                Show this help\n");
    printf("\n");
    printf("Output structure:\n");
    printf("  mirror/\n");
    printf("  |- manifest.json        # Package -> hash mapping\n");
    printf("  |- archives/\n");
    printf("  |  |- {sha1}.zip        # Content-addressable archives\n");
    printf("  |- packages/\n");
    printf("     |- {author}/\n");
    printf("        |- {name}/\n");
    printf("           |- {version}/\n");
    printf("              |- elm.json\n");
    printf("              |- docs.json\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s repository mirror                           # Incremental sync\n", prog);
    printf("  %s repository mirror --full                    # Full re-sync\n", prog);
    printf("  %s repository mirror --package elm/core -v     # Mirror single package\n", prog);
    printf("  %s repository mirror --dry-run                 # Preview what would sync\n", prog);
    printf("  %s repository mirror --latest-only             # Only latest versions\n", prog);
}

int cmd_mirror(int argc, char *argv[]) {
    MirrorOptions opts = {
        .output_dir = DEFAULT_OUTPUT_DIR,
        .manifest_path = NULL,  /* Will be set from output_dir if not specified */
        .fail_log_path = NULL,
        .full_sync = false,
        .latest_only = false,
        .dry_run = false,
        .confirm = true,
        .verbose = false,
        .quiet = false,
        .specific_packages = NULL,
        .specific_package_count = 0,
        .specific_package_capacity = 0
    };

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            opts.confirm = false;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opts.quiet = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts.verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            opts.dry_run = true;
        } else if (strcmp(argv[i], "--full") == 0) {
            opts.full_sync = true;
        } else if (strcmp(argv[i], "--latest-only") == 0) {
            opts.latest_only = true;
        } else if (strcmp(argv[i], "--output-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --output-dir requires a path\n");
                return 1;
            }
            opts.output_dir = argv[++i];
        } else if (strcmp(argv[i], "--manifest") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --manifest requires a path\n");
                return 1;
            }
            opts.manifest_path = argv[++i];
        } else if (strcmp(argv[i], "--fail-log") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --fail-log requires a file path\n");
                return 1;
            }
            opts.fail_log_path = argv[++i];
        } else if (strcmp(argv[i], "--package") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --package requires author/name\n");
                return 1;
            }
            i++;
            /* Validate format */
            if (!strchr(argv[i], '/')) {
                fprintf(stderr, "Error: Invalid package format '%s', expected author/name\n", argv[i]);
                return 1;
            }
            /* Add to filter list */
            if (opts.specific_package_count >= opts.specific_package_capacity) {
                size_t new_cap = opts.specific_package_capacity == 0 ? 8 : opts.specific_package_capacity * 2;
                char **new_pkgs = arena_realloc(opts.specific_packages, sizeof(char*) * new_cap);
                if (!new_pkgs) {
                    fprintf(stderr, "Error: Memory allocation failed\n");
                    return 1;
                }
                opts.specific_packages = new_pkgs;
                opts.specific_package_capacity = new_cap;
            }
            opts.specific_packages[opts.specific_package_count++] = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 1;
        } else {
            /* Positional argument: output directory */
            opts.output_dir = argv[i];
        }
    }

    /* Build manifest path if not specified */
    char manifest_path_buf[MAX_PATH_LENGTH];
    if (!opts.manifest_path) {
        snprintf(manifest_path_buf, sizeof(manifest_path_buf), "%s/%s",
                 opts.output_dir, DEFAULT_MANIFEST_FILE);
        opts.manifest_path = manifest_path_buf;
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

    /* Set longer timeout for bulk downloads */
    curl_session_set_timeout(env->curl_session, 60000L);

    if (!opts.quiet) {
        printf("\n%s-- MIRROR --%s\n\n", ANSI_CYAN, ANSI_RESET);
        printf("Registry: %s\n", env->cache->registry_path);
        printf("Output:   %s\n", opts.output_dir);
        printf("Manifest: %s\n", opts.manifest_path);
        printf("Packages in registry: %zu\n\n", env->registry->entry_count);
    }

    /* Create output directories */
    size_t archives_dir_len = strlen(opts.output_dir) + strlen("/archives") + 1;
    char *archives_dir = arena_malloc(archives_dir_len);
    size_t packages_dir_len = strlen(opts.output_dir) + strlen("/packages") + 1;
    char *packages_dir = arena_malloc(packages_dir_len);

    if (!archives_dir || !packages_dir) {
        log_error("Memory allocation failed");
        install_env_free(env);
        return 1;
    }

    snprintf(archives_dir, archives_dir_len, "%s/archives", opts.output_dir);
    snprintf(packages_dir, packages_dir_len, "%s/packages", opts.output_dir);

    if (!opts.dry_run) {
        if (!mkdir_p(archives_dir) || !mkdir_p(packages_dir)) {
            log_error("Failed to create output directories");
            arena_free(archives_dir);
            arena_free(packages_dir);
            install_env_free(env);
            return 1;
        }
    }

    /* Load blacklist */
    Blacklist *blacklist = blacklist_load(opts.verbose);

    /* Load last-processed sequence number */
    size_t last_processed = 0;
    if (!opts.full_sync) {
        mirror_since_read(&last_processed);
        if (opts.verbose) {
            printf("Last processed sequence: %zu\n", last_processed);
        }
    }

    /* Load existing manifest (if exists) */
    MirrorManifest *manifest = mirror_manifest_load_json(opts.manifest_path);
    if (!manifest) {
        manifest = mirror_manifest_create();
        if (!manifest) {
            log_error("Failed to create manifest");
            blacklist_free(blacklist);
            arena_free(archives_dir);
            arena_free(packages_dir);
            install_env_free(env);
            return 1;
        }
    }

    /* Create work queue */
    MirrorQueue *queue = queue_create();
    if (!queue) {
        log_error("Failed to create work queue");
        mirror_manifest_free(manifest);
        blacklist_free(blacklist);
        arena_free(archives_dir);
        arena_free(packages_dir);
        install_env_free(env);
        return 1;
    }

    MirrorStats stats = {0};

    /* Open fail log if specified */
    FILE *fail_log = NULL;
    if (opts.fail_log_path && !opts.dry_run) {
        fail_log = fopen(opts.fail_log_path, "w");
        if (!fail_log) {
            fprintf(stderr, "Error: Cannot open fail log file: %s\n", opts.fail_log_path);
            queue_free(queue);
            mirror_manifest_free(manifest);
            blacklist_free(blacklist);
            arena_free(archives_dir);
            arena_free(packages_dir);
            install_env_free(env);
            return 1;
        }
        fprintf(fail_log, "# Failed packages from mirror\n");
        fprintf(fail_log, "# Can be used as blacklist.txt\n\n");
        fflush(fail_log);
    }

    /* Phase 1: Build work queue */
    if (!opts.quiet) {
        printf("Scanning registry...\n");
    }

    size_t current_seq = 0;  /* Track sequence for incremental sync */

    for (size_t pkg_idx = 0; pkg_idx < env->registry->entry_count; pkg_idx++) {
        RegistryEntry *entry = &env->registry->entries[pkg_idx];
        stats.total_packages++;

        /* Apply package filter */
        if (!is_package_in_filter(&opts, entry->author, entry->name)) {
            continue;
        }

        size_t versions_to_check = opts.latest_only ? 1 : entry->version_count;

        for (size_t ver_idx = 0; ver_idx < versions_to_check && ver_idx < entry->version_count; ver_idx++) {
            char *ver_str = version_to_string(&entry->versions[ver_idx]);
            if (!ver_str) continue;

            current_seq++;
            stats.total_versions++;

            /* Skip if blacklisted */
            if (blacklist_contains(blacklist, entry->author, entry->name, ver_str)) {
                stats.skipped++;
                if (opts.verbose) {
                    printf("  %s/%s %s SKIPPED (blacklisted)\n",
                           entry->author, entry->name, ver_str);
                }
                arena_free(ver_str);
                continue;
            }

            /* Skip if already processed (unless --full) */
            if (!opts.full_sync && current_seq <= last_processed) {
                stats.already_mirrored++;
                if (opts.verbose) {
                    printf("  %s%s/%s %s%s ALREADY PROCESSED\n",
                           ANSI_GREEN, entry->author, entry->name, ver_str, ANSI_RESET);
                }
                arena_free(ver_str);
                continue;
            }

            /* Check if already in manifest */
            const char *existing_hash = mirror_manifest_lookup(manifest, entry->author,
                                                                entry->name, ver_str);
            if (existing_hash && !opts.full_sync) {
                stats.already_mirrored++;
                if (opts.verbose) {
                    printf("  %s%s/%s %s%s IN MANIFEST\n",
                           ANSI_GREEN, entry->author, entry->name, ver_str, ANSI_RESET);
                }
                arena_free(ver_str);
                continue;
            }

            /* Add to queue */
            queue_add(queue, entry->author, entry->name, ver_str, current_seq);
            if (opts.verbose) {
                printf("  %s%s/%s %s%s QUEUED\n",
                       ANSI_YELLOW, entry->author, entry->name, ver_str, ANSI_RESET);
            }

            arena_free(ver_str);
        }
    }

    /* Report scan results */
    printf("\n%s-- SCAN COMPLETE --%s\n", ANSI_CYAN, ANSI_RESET);
    printf("Total packages:    %zu\n", stats.total_packages);
    printf("Total versions:    %zu\n", stats.total_versions);
    printf("Already mirrored:  %s%zu%s\n", ANSI_GREEN, stats.already_mirrored, ANSI_RESET);
    if (stats.skipped > 0) {
        printf("Skipped:           %zu (blacklisted)\n", stats.skipped);
    }
    printf("To mirror:         %zu\n", queue->count);

    /* Nothing to mirror? */
    if (queue->count == 0) {
        printf("\n%sAll packages are already mirrored!%s\n", ANSI_GREEN, ANSI_RESET);
        if (fail_log) fclose(fail_log);
        queue_free(queue);
        mirror_manifest_free(manifest);
        blacklist_free(blacklist);
        arena_free(archives_dir);
        arena_free(packages_dir);
        install_env_free(env);
        return 0;
    }

    /* Dry run - show what would be mirrored */
    if (opts.dry_run) {
        if (!opts.quiet) {
            printf("\nWould mirror:\n");
            for (size_t i = 0; i < queue->count; i++) {
                MirrorItem *item = &queue->items[i];
                printf("  %s/%s %s\n", item->author, item->name, item->version);
            }
        }
        printf("\n");
        if (fail_log) fclose(fail_log);
        queue_free(queue);
        mirror_manifest_free(manifest);
        blacklist_free(blacklist);
        arena_free(archives_dir);
        arena_free(packages_dir);
        install_env_free(env);
        return 0;
    }

    /* Confirm with user */
    if (opts.confirm) {
        printf("Mirror %zu package version(s)? [y/N] ", queue->count);
        fflush(stdout);

        char response[16];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'y' && response[0] != 'Y')) {
            printf("Aborted.\n");
            if (fail_log) fclose(fail_log);
            queue_free(queue);
            mirror_manifest_free(manifest);
            blacklist_free(blacklist);
            arena_free(archives_dir);
            arena_free(packages_dir);
            install_env_free(env);
            return 0;
        }
    }

    /* Phase 2: Mirror packages */
    if (!opts.quiet) {
        printf("\n%s-- MIRRORING --%s\n\n", ANSI_CYAN, ANSI_RESET);
    }

    /* Seed random for delays */
    srand((unsigned int)time(NULL));

    size_t max_seq_processed = last_processed;

    for (size_t i = 0; i < queue->count; i++) {
        MirrorItem *item = &queue->items[i];

        if (!opts.quiet) {
            printf("[%zu/%zu] %s/%s %s ", i + 1, queue->count,
                   item->author, item->name, item->version);
            fflush(stdout);
        }

        /* Fetch metadata from registry */
        if (!fetch_package_metadata(env, item->author, item->name, item->version)) {
            stats.download_failed++;
            if (!opts.quiet) {
                printf("%sFAILED%s (metadata fetch)\n", ANSI_RED, ANSI_RESET);
            }
            if (fail_log) {
                fprintf(fail_log, "%s/%s@%s  # metadata fetch failed\n",
                        item->author, item->name, item->version);
                fflush(fail_log);
            }
            continue;
        }

        /* Read endpoint.json to get URL and expected hash */
        char *endpoint_path = build_package_file_path(env->cache->packages_dir,
                                                       item->author, item->name,
                                                       item->version, "endpoint.json");
        if (!endpoint_path) {
            stats.download_failed++;
            if (!opts.quiet) {
                printf("%sFAILED%s (endpoint path)\n", ANSI_RED, ANSI_RESET);
            }
            continue;
        }

        char *endpoint_data = file_read_contents_bounded(endpoint_path, MAX_LARGE_BUFFER_LENGTH, NULL);
        arena_free(endpoint_path);

        if (!endpoint_data) {
            stats.download_failed++;
            if (!opts.quiet) {
                printf("%sFAILED%s (endpoint read)\n", ANSI_RED, ANSI_RESET);
            }
            continue;
        }

        PackageEndpoint *endpoint = package_endpoint_parse(endpoint_data);
        arena_free(endpoint_data);

        if (!endpoint) {
            stats.download_failed++;
            if (!opts.quiet) {
                printf("%sFAILED%s (endpoint parse)\n", ANSI_RED, ANSI_RESET);
            }
            continue;
        }

        /* Download archive with retry */
        char *archive_path = NULL;
        int backoff_secs = INITIAL_BACKOFF_SECS;

        for (int attempt = 0; attempt < MAX_RETRIES && !archive_path; attempt++) {
            if (attempt > 0) {
                if (!opts.quiet) {
                    printf("retry %d/%d after %ds... ", attempt, MAX_RETRIES - 1, backoff_secs);
                    fflush(stdout);
                }
                sleep((unsigned int)backoff_secs);
                backoff_secs *= 2;
            }
            archive_path = fetch_package_archive(env, item->author, item->name,
                                                  item->version, endpoint);
        }

        if (!archive_path) {
            stats.download_failed++;
            if (!opts.quiet) {
                printf("%sFAILED%s (download)\n", ANSI_RED, ANSI_RESET);
            }
            if (fail_log) {
                fprintf(fail_log, "%s/%s@%s  # download failed\n",
                        item->author, item->name, item->version);
                fflush(fail_log);
            }
            package_endpoint_free(endpoint);
            continue;
        }

        /* Store archive by hash */
        if (!store_archive_by_hash(archive_path, endpoint->hash, archives_dir)) {
            stats.download_failed++;
            if (!opts.quiet) {
                printf("%sFAILED%s (store archive)\n", ANSI_RED, ANSI_RESET);
            }
            remove(archive_path);
            arena_free(archive_path);
            package_endpoint_free(endpoint);
            continue;
        }

        /* Store metadata files */
        if (!store_metadata(env, item->author, item->name, item->version, packages_dir)) {
            /* Non-fatal - continue anyway */
            if (opts.verbose) {
                printf("(metadata store failed) ");
            }
        }

        /* Add to manifest */
        mirror_manifest_add(manifest, item->author, item->name, item->version,
                            endpoint->hash, endpoint->url);

        /* Clean up temp file */
        remove(archive_path);
        arena_free(archive_path);
        package_endpoint_free(endpoint);

        stats.downloaded_ok++;
        if (!opts.quiet) {
            printf("%sOK%s\n", ANSI_GREEN, ANSI_RESET);
        }

        /* Track highest processed sequence */
        if (item->seq > max_seq_processed) {
            max_seq_processed = item->seq;
        }

        /* Random delay between requests */
        if (i < queue->count - 1) {
            random_delay();
        }
    }

    /* Update manifest metadata */
    char *timestamp = get_iso8601_timestamp();
    if (timestamp) {
        mirror_manifest_set_generated(manifest, timestamp);
        arena_free(timestamp);
    }
    mirror_manifest_set_source(manifest, "package.elm-lang.org");

    /* Write manifest */
    if (!mirror_manifest_write_json(manifest, opts.manifest_path)) {
        fprintf(stderr, "Warning: Failed to write manifest to %s\n", opts.manifest_path);
    }

    /* Write new last-processed sequence */
    if (max_seq_processed > last_processed) {
        mirror_since_write(max_seq_processed);
    }

    /* Final summary */
    printf("\n%s-- SUMMARY --%s\n", ANSI_CYAN, ANSI_RESET);
    printf("Mirrored:          %s%zu%s\n", ANSI_GREEN, stats.downloaded_ok, ANSI_RESET);
    if (stats.download_failed > 0) {
        printf("Failed:            %s%zu%s\n", ANSI_RED, stats.download_failed, ANSI_RESET);
        if (fail_log) {
            printf("Failures logged to: %s\n", opts.fail_log_path);
        }
    }

    if (fail_log) fclose(fail_log);
    queue_free(queue);
    mirror_manifest_free(manifest);
    blacklist_free(blacklist);
    arena_free(archives_dir);
    arena_free(packages_dir);
    install_env_free(env);

    return stats.download_failed > 0 ? 1 : 0;
}
