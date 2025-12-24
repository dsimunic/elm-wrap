/**
 * repository.c - Repository command group for managing local package repositories
 *
 * This command provides utilities for creating and listing local package
 * repository directories organized by compiler name and version.
 */

#include "repository.h"
#include "../package/install_local_dev.h"
#include "../package/package_common.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../global_context.h"
#include "../../env_defaults.h"
#include "../../elm_compiler.h"
#include "../../elm_json.h"
#include "../../log.h"
#include "../../protocol_v2/index_fetch.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../registry.h"
#include "../../cache.h"
#include "../../rulr/rulr.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/host_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH_LENGTH
#endif

/* ============================================================================
 * Usage
 * ========================================================================== */

static int is_help_flag(const char *arg) {
    return arg && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0);
}

static void print_repository_usage(void) {
    printf("Usage: %s repository SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Manage local package repositories.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  init [ROOT_PATH]      Create a new repository directory\n");
    printf("  list [ROOT_PATH]      List repositories at path\n");
    printf("  local-dev             Manage local development tracking\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
}

static void print_new_usage(void) {
    printf("Usage: %s repository init [ROOT_PATH] [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Create a new repository directory for the current (or specified) compiler.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  ROOT_PATH             Root path for repositories (default: WRAP_REPOSITORY_LOCAL_PATH)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --compiler NAME       Compiler name (elm, lamdera, wrapc, etc.)\n");
    printf("  --version VERSION     Compiler version (e.g., 0.19.1)\n");
    printf("  -h, --help            Show this help message\n");
    printf("\n");
    printf("The repository path is: ROOT_PATH/NAME/VERSION\n");
    printf("For example: ~/.elm-wrap/repository/elm/0.19.1/\n");
}

static void print_list_usage(void) {
    printf("Usage: %s repository list [ROOT_PATH]\n", global_context_program_name());
    printf("\n");
    printf("List all repositories at the given path.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  ROOT_PATH             Root path for repositories (default: WRAP_REPOSITORY_LOCAL_PATH)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
}

static void print_local_dev_usage(void) {
    printf("Usage: %s repository local-dev [COMMAND]\n", global_context_program_name());
    printf("\n");
    printf("Manage local development package tracking.\n");
    printf("\n");
    printf("Commands:\n");
    printf("  (no command)          List all tracked local-dev packages and their dependents\n");
    printf("  clear --all           Clear all dependency tracking\n");
    printf("  clear PACKAGE VERSION\n");
    printf("                        Clear tracking for a specific package version\n");
    printf("  clear PACKAGE VERSION PATH\n");
    printf("                        Clear tracking for a specific path only\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
}

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static void remove_local_dev_from_v1_registry_dat(const char *author, const char *name, const char *version) {
    if (!author || !name || !version) {
        return;
    }

    CacheConfig *cache = cache_config_init();
    if (!cache || !cache->registry_path) {
        if (cache) cache_config_free(cache);
        return;
    }

    const char *registry_path = cache->registry_path;
    struct stat st;
    if (stat(registry_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cache_config_free(cache);
        return;
    }

    Registry *registry = registry_load_from_dat(registry_path, NULL);
    if (!registry) {
        cache_config_free(cache);
        return;
    }

    Version parsed = version_parse(version);
    bool removed = false;
    if (!registry_remove_version_ex(registry, author, name, parsed, false, &removed)) {
        registry_free(registry);
        cache_config_free(cache);
        return;
    }

    if (removed) {
        registry_sort_entries(registry);
        registry_dat_write(registry, registry_path);
    }

    registry_free(registry);
    cache_config_free(cache);
}

/**
 * Get the compiler name from the compiler path.
 * Extracts the basename of the compiler path.
 * Returns "elm" if no custom path is set.
 */
static char *get_compiler_name(void) {
    const char *compiler_path = getenv("WRAP_ELM_COMPILER_PATH");
    if (compiler_path && compiler_path[0] != '\0') {
        /* Make a copy to use basename */
        char *path_copy = arena_strdup(compiler_path);
        if (!path_copy) {
            return arena_strdup("elm");
        }
        char *base = basename(path_copy);
        char *result = arena_strdup(base);
        arena_free(path_copy);
        return result;
    }
    return arena_strdup("elm");
}

/**
 * Create a directory and all parent directories (like mkdir -p).
 * Returns 0 on success, -1 on failure.
 */
static int mkdir_p(const char *path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0; /* Directory already exists */
        }
        errno = EEXIST;
        return -1; /* Exists but is not a directory */
    }

    /* Create parent directory first */
    char *path_copy = arena_strdup(path);
    if (!path_copy) {
        return -1;
    }

    char *parent = dirname(path_copy);
    /* Make a copy of parent since dirname modifies the string in place */
    char *parent_copy = arena_strdup(parent);
    arena_free(path_copy);
    
    if (!parent_copy) {
        return -1;
    }

    if (strcmp(parent_copy, ".") != 0 && strcmp(parent_copy, "/") != 0 && strcmp(parent_copy, path) != 0) {
        if (mkdir_p(parent_copy) != 0) {
            arena_free(parent_copy);
            return -1;
        }
    }
    arena_free(parent_copy);

    /* Create this directory */
    if (mkdir(path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Subcommands
 * ========================================================================== */

int cmd_repository_new(int argc, char *argv[]) {
    const char *root_path = NULL;
    const char *compiler_name = NULL;
    const char *compiler_version = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_new_usage();
            return 0;
        } else if (strcmp(argv[i], "--compiler") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --compiler requires a value\n");
                return 1;
            }
            compiler_name = argv[++i];
        } else if (strcmp(argv[i], "--version") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --version requires a value\n");
                return 1;
            }
            compiler_version = argv[++i];
        } else if (argv[i][0] != '-') {
            if (root_path == NULL) {
                root_path = argv[i];
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    /* Get root path (default from environment/compiled defaults) */
    char *effective_root = NULL;
    if (root_path) {
        /* Expand tilde if present */
        if (root_path[0] == '~') {
            const char *home = getenv("HOME");
            if (home) {
                size_t len = strlen(home) + strlen(root_path);
                effective_root = arena_malloc(len);
                if (!effective_root) {
                    log_error("Out of memory");
                    return 1;
                }
                snprintf(effective_root, len, "%s%s", home, root_path + 1);
            } else {
                effective_root = arena_strdup(root_path);
            }
        } else {
            effective_root = arena_strdup(root_path);
        }
    } else {
        effective_root = env_get_repository_local_path();
    }

    if (!effective_root || effective_root[0] == '\0') {
        fprintf(stderr, "Error: Could not determine repository root path\n");
        fprintf(stderr, "Set WRAP_REPOSITORY_LOCAL_PATH or provide a path argument\n");
        return 1;
    }

    /* Get compiler name (from argument, WRAP_ELM_COMPILER_PATH basename, or "elm") */
    char *effective_compiler = NULL;
    if (compiler_name) {
        effective_compiler = arena_strdup(compiler_name);
    } else {
        effective_compiler = get_compiler_name();
    }

    if (!effective_compiler) {
        log_error("Out of memory");
        return 1;
    }

    /* Get compiler version (from argument or by running compiler --version) */
    char *effective_version = NULL;
    if (compiler_version) {
        effective_version = arena_strdup(compiler_version);
    } else {
        effective_version = elm_compiler_get_version();
    }

    if (!effective_version) {
        fprintf(stderr, "Error: Could not determine compiler version\n");
        fprintf(stderr, "Use --version to specify it manually, or ensure the compiler is in PATH\n");
        return 1;
    }

    /* Build the full repository path */
    size_t path_len = strlen(effective_root) + 1 + strlen(effective_compiler) + 1 + strlen(effective_version) + 2;
    char *repo_path = arena_malloc(path_len);
    if (!repo_path) {
        log_error("Out of memory");
        return 1;
    }
    snprintf(repo_path, path_len, "%s/%s/%s", effective_root, effective_compiler, effective_version);

    /* Check if repository already exists */
    struct stat st;
    if (stat(repo_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Check if index.dat exists */
        size_t index_path_len = strlen(repo_path) + strlen("/index.dat") + 1;
        char *index_path = arena_malloc(index_path_len);
        if (index_path) {
            snprintf(index_path, index_path_len, "%s/index.dat", repo_path);
            if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
                printf("Repository already exists: %s\n", repo_path);
                arena_free(index_path);
                return 0;
            }
            arena_free(index_path);
        }
    }

    /* Create the directory */
    log_debug("Creating repository at: %s", repo_path);

    if (mkdir_p(repo_path) != 0) {
        fprintf(stderr, "Error: Failed to create directory '%s': %s\n", repo_path, strerror(errno));
        return 1;
    }

    printf("Created repository: %s\n", repo_path);

    /* Download the registry index */
    if (!v2_index_fetch(repo_path, effective_compiler, effective_version)) {
        fprintf(stderr, "Warning: Failed to download registry index\n");
        /* Continue anyway - the directory was created successfully */
    }

    return 0;
}

int cmd_repository_list(int argc, char *argv[]) {
    const char *root_path = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_list_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            if (root_path == NULL) {
                root_path = argv[i];
            } else {
                fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    /* Get root path (default from environment/compiled defaults) */
    char *effective_root = NULL;
    if (root_path) {
        /* Expand tilde if present */
        if (root_path[0] == '~') {
            const char *home = getenv("HOME");
            if (home) {
                size_t len = strlen(home) + strlen(root_path);
                effective_root = arena_malloc(len);
                if (!effective_root) {
                    log_error("Out of memory");
                    return 1;
                }
                snprintf(effective_root, len, "%s%s", home, root_path + 1);
            } else {
                effective_root = arena_strdup(root_path);
            }
        } else {
            effective_root = arena_strdup(root_path);
        }
    } else {
        effective_root = env_get_repository_local_path();
    }

    if (!effective_root || effective_root[0] == '\0') {
        fprintf(stderr, "Error: Could not determine repository root path\n");
        fprintf(stderr, "Set WRAP_HOME or provide a path argument\n");
        return 1;
    }

    /* Check if root path exists */
    struct stat st;
    if (stat(effective_root, &st) != 0) {
        if (errno == ENOENT) {
            printf("No repositories found (directory does not exist: %s)\n", effective_root);
        } else {
            fprintf(stderr, "Error: Cannot access '%s': %s\n", effective_root, strerror(errno));
        }
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a directory\n", effective_root);
        return 1;
    }

    /* List compiler directories */
    DIR *root_dir = opendir(effective_root);
    if (!root_dir) {
        fprintf(stderr, "Error: Cannot open directory '%s': %s\n", effective_root, strerror(errno));
        return 1;
    }

    printf("Repositories at %s:\n", effective_root);

    int found_any = 0;
    struct dirent *compiler_entry;
    while ((compiler_entry = readdir(root_dir)) != NULL) {
        /* Skip . and .. and directories starting with _ */
        if (compiler_entry->d_name[0] == '.' || compiler_entry->d_name[0] == '_') {
            continue;
        }

        /* Build path to compiler directory */
        size_t compiler_path_len = strlen(effective_root) + 1 + strlen(compiler_entry->d_name) + 1;
        char *compiler_path = arena_malloc(compiler_path_len);
        if (!compiler_path) {
            closedir(root_dir);
            log_error("Out of memory");
            return 1;
        }
        snprintf(compiler_path, compiler_path_len, "%s/%s", effective_root, compiler_entry->d_name);

        /* Check if it's a directory */
        struct stat compiler_st;
        if (stat(compiler_path, &compiler_st) != 0 || !S_ISDIR(compiler_st.st_mode)) {
            arena_free(compiler_path);
            continue;
        }

        /* List version directories under this compiler */
        DIR *compiler_dir = opendir(compiler_path);
        if (!compiler_dir) {
            arena_free(compiler_path);
            continue;
        }

        struct dirent *version_entry;
        while ((version_entry = readdir(compiler_dir)) != NULL) {
            /* Skip . and .. */
            if (version_entry->d_name[0] == '.') {
                continue;
            }

            /* Build path to version directory */
            size_t version_path_len = strlen(compiler_path) + 1 + strlen(version_entry->d_name) + 1;
            char *version_path = arena_malloc(version_path_len);
            if (!version_path) {
                closedir(compiler_dir);
                arena_free(compiler_path);
                closedir(root_dir);
                log_error("Out of memory");
                return 1;
            }
            snprintf(version_path, version_path_len, "%s/%s", compiler_path, version_entry->d_name);

            /* Check if it's a directory */
            struct stat version_st;
            if (stat(version_path, &version_st) == 0 && S_ISDIR(version_st.st_mode)) {
                printf("  %s/%s\n", compiler_entry->d_name, version_entry->d_name);
                found_any = 1;
            }

            arena_free(version_path);
        }

        closedir(compiler_dir);
        arena_free(compiler_path);
    }

    closedir(root_dir);

    if (!found_any) {
        printf("  (no repositories found)\n");
    }

    return 0;
}

/* ============================================================================
 * Local-dev Subcommand
 * ========================================================================== */

typedef struct {
    char *author;
    char *name;
    char *version;
    char *app_path;
} LocalDevConnection;

static void prune_stale_local_dev_connections(LocalDevConnection *connections, int connection_count);

static bool has_processed_app_path(LocalDevConnection *connections, int index) {
    if (!connections || index <= 0) {
        return false;
    }

    const char *path = connections[index].app_path;
    for (int i = 0; i < index; i++) {
        if (strcmp(connections[i].app_path, path) == 0) {
            return true;
        }
    }
    return false;
}

static void insert_app_dependencies_for_path(Rulr *rulr, const char *app_path) {
    if (!rulr || !app_path) {
        return;
    }

    ElmJson *elm_json = elm_json_read(app_path);
    if (!elm_json) {
        log_debug("Failed to read tracked application elm.json: %s", app_path);
        return;
    }

    if (elm_json->type != ELM_PROJECT_APPLICATION) {
        log_debug("Skipping non-application project for local-dev pruning: %s", app_path);
        elm_json_free(elm_json);
        return;
    }

    if (elm_json->dependencies_direct) {
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            rulr_insert_fact_3s(rulr, "app_dependency", app_path, pkg->author, pkg->name);
        }
    }

    if (elm_json->dependencies_test_direct) {
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            rulr_insert_fact_3s(rulr, "app_dependency", app_path, pkg->author, pkg->name);
        }
    }

    elm_json_free(elm_json);
}

/**
 * Recursively remove a directory and its contents.
 */
static bool remove_directory_recursive_local(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        return unlink(path) == 0;
    }

    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(dir)) != NULL && ok) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t sub_len = strlen(path) + strlen(entry->d_name) + 2;
        char *sub_path = arena_malloc(sub_len);
        if (!sub_path) {
            ok = false;
            break;
        }
        snprintf(sub_path, sub_len, "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(sub_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ok = remove_directory_recursive_local(sub_path);
        } else {
            ok = unlink(sub_path) == 0;
        }
        arena_free(sub_path);
    }

    closedir(dir);
    if (ok) {
        ok = rmdir(path) == 0;
    }
    return ok;
}

/**
 * Get the path to the local-dev registry file.
 * Uses the tracking directory (V2-independent) as the base location.
 */
static char *get_local_dev_registry_path(void) {
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return NULL;
    }
    
    /* Build path: tracking_dir/registry-local-dev.dat */
    size_t path_len = strlen(tracking_dir) + strlen("/") + strlen(REGISTRY_LOCAL_DEV_DAT) + 1;
    char *reg_path = arena_malloc(path_len);
    if (reg_path) {
        snprintf(reg_path, path_len, "%s/%s", tracking_dir, REGISTRY_LOCAL_DEV_DAT);
    }
    arena_free(tracking_dir);
    
    return reg_path;
}

/**
 * List tracking entries for a specific package version.
 * Returns the count of tracking entries found.
 */
static int list_tracking_for_package(const char *tracking_dir, const char *author, const char *name,
                                     const char *version, LocalDevConnection **connections,
                                     int *connection_count, int *connection_capacity,
                                     bool *connection_tracking_enabled) {
    struct stat st;
    int found_count = 0;

    /* Build path: tracking_dir/author/name/version */
    size_t version_path_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *version_path = arena_malloc(version_path_len);
    if (!version_path) return 0;
    snprintf(version_path, version_path_len, "%s/%s/%s/%s", tracking_dir, author, name, version);

    if (stat(version_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        arena_free(version_path);
        return 0;
    }

    DIR *track_files_handle = opendir(version_path);
    if (!track_files_handle) {
        arena_free(version_path);
        return 0;
    }

    struct dirent *track_entry;
    while ((track_entry = readdir(track_files_handle)) != NULL) {
        if (track_entry->d_name[0] == '.') continue;

        size_t track_file_len = strlen(version_path) + strlen(track_entry->d_name) + 2;
        char *track_file = arena_malloc(track_file_len);
        if (!track_file) continue;
        snprintf(track_file, track_file_len, "%s/%s", version_path, track_entry->d_name);

        FILE *f = fopen(track_file, "r");
        if (f) {
            char path_buf[PATH_MAX];
            if (fgets(path_buf, sizeof(path_buf), f)) {
                size_t len = strlen(path_buf);
                while (len > 0 && (path_buf[len - 1] == '\n' || path_buf[len - 1] == '\r')) {
                    path_buf[--len] = '\0';
                }
                printf("    -> %s\n", path_buf);
                found_count++;

                if (*connection_tracking_enabled && connections && connection_count && connection_capacity) {
                    if (*connection_count >= *connection_capacity) {
                        int new_capacity = *connection_capacity * 2;
                        LocalDevConnection *new_connections =
                            arena_realloc(*connections, new_capacity * sizeof(LocalDevConnection));
                        if (!new_connections) {
                            *connection_tracking_enabled = false;
                            log_error("Out of memory tracking local-dev connections; automatic pruning disabled.");
                        } else {
                            *connections = new_connections;
                            *connection_capacity = new_capacity;
                        }
                    }

                    if (*connection_tracking_enabled) {
                        char *author_copy = arena_strdup(author);
                        char *name_copy = arena_strdup(name);
                        char *version_copy = arena_strdup(version);
                        char *path_copy = arena_strdup(path_buf);
                        if (!author_copy || !name_copy || !version_copy || !path_copy) {
                            *connection_tracking_enabled = false;
                            log_error("Out of memory copying local-dev tracking data; automatic pruning disabled.");
                        } else {
                            LocalDevConnection *conn = &(*connections)[(*connection_count)++];
                            conn->author = author_copy;
                            conn->name = name_copy;
                            conn->version = version_copy;
                            conn->app_path = path_copy;
                        }
                    }
                }
            }
            fclose(f);
        }
        arena_free(track_file);
    }
    closedir(track_files_handle);
    arena_free(version_path);

    return found_count;
}

/**
 * List all registered local-dev packages and their dependent applications.
 */
static int list_local_dev_tracking(void) {
    char *tracking_dir = get_local_dev_tracking_dir();
    char *registry_path = get_local_dev_registry_path();

    int connection_capacity = INITIAL_CONNECTION_CAPACITY;
    int connection_count = 0;
    bool connection_tracking_enabled = true;
    LocalDevConnection *connections = arena_malloc(connection_capacity * sizeof(LocalDevConnection));
    if (!connections) {
        connection_tracking_enabled = false;
        log_error("Out of memory preparing local-dev pruning buffer; automatic pruning disabled.");
    }

    int found_any = 0;

    /* Load the local-dev registry to get all registered packages */
    V2Registry *local_dev_registry = NULL;
    if (registry_path) {
        struct stat st;
        if (stat(registry_path, &st) == 0 && S_ISREG(st.st_mode)) {
            local_dev_registry = v2_registry_load_from_text(registry_path);
        }
    }

    if (local_dev_registry && local_dev_registry->entry_count > 0) {
        printf("Tracked local-dev packages:\n\n");
        found_any = 1;

        /* Iterate over all packages in the registry */
        for (size_t i = 0; i < local_dev_registry->entry_count; i++) {
            V2PackageEntry *entry = &local_dev_registry->entries[i];

            /* Iterate over all versions */
            for (size_t j = 0; j < entry->version_count; j++) {
                V2PackageVersion *ver = &entry->versions[j];
                char *version_str = version_format(ver->major, ver->minor, ver->patch);
                if (!version_str) continue;

                printf("  %s/%s %s\n", entry->author, entry->name, version_str);

                /* Show tracking entries for this package version if tracking_dir exists */
                if (tracking_dir) {
                    list_tracking_for_package(tracking_dir, entry->author, entry->name, version_str,
                                              &connections, &connection_count, &connection_capacity,
                                              &connection_tracking_enabled);
                }
                printf("\n");
                arena_free(version_str);
            }
        }

        v2_registry_free(local_dev_registry);
    }

    /* Also check for packages in tracking directory that might not be in the registry
     * (e.g., if registry was manually edited or corrupted) */
    if (tracking_dir) {
        struct stat st;
        if (stat(tracking_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *author_dir_handle = opendir(tracking_dir);
            if (author_dir_handle) {
                struct dirent *author_entry;
                while ((author_entry = readdir(author_dir_handle)) != NULL) {
                    if (author_entry->d_name[0] == '.') continue;

                    size_t author_path_len = strlen(tracking_dir) + strlen(author_entry->d_name) + 2;
                    char *author_path = arena_malloc(author_path_len);
                    if (!author_path) continue;
                    snprintf(author_path, author_path_len, "%s/%s", tracking_dir, author_entry->d_name);

                    if (stat(author_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                        arena_free(author_path);
                        continue;
                    }

                    DIR *name_dir_handle = opendir(author_path);
                    if (!name_dir_handle) {
                        arena_free(author_path);
                        continue;
                    }

                    struct dirent *name_entry;
                    while ((name_entry = readdir(name_dir_handle)) != NULL) {
                        if (name_entry->d_name[0] == '.') continue;

                        size_t name_path_len = strlen(author_path) + strlen(name_entry->d_name) + 2;
                        char *name_path = arena_malloc(name_path_len);
                        if (!name_path) continue;
                        snprintf(name_path, name_path_len, "%s/%s", author_path, name_entry->d_name);

                        if (stat(name_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                            arena_free(name_path);
                            continue;
                        }

                        DIR *version_dir_handle = opendir(name_path);
                        if (!version_dir_handle) {
                            arena_free(name_path);
                            continue;
                        }

                        struct dirent *version_entry;
                        while ((version_entry = readdir(version_dir_handle)) != NULL) {
                            if (version_entry->d_name[0] == '.') continue;

                            /* Check if this package/version was already shown from the registry */
                            bool already_shown = false;
                            if (local_dev_registry) {
                                V2PackageEntry *reg_entry = v2_registry_find(local_dev_registry,
                                                                              author_entry->d_name,
                                                                              name_entry->d_name);
                                if (reg_entry) {
                                    /* Parse version string */
                                    Version parsed_v;
                                    if (version_parse_safe(version_entry->d_name, &parsed_v)) {
                                        for (size_t v = 0; v < reg_entry->version_count; v++) {
                                            if (reg_entry->versions[v].major == parsed_v.major &&
                                                reg_entry->versions[v].minor == parsed_v.minor &&
                                                reg_entry->versions[v].patch == parsed_v.patch) {
                                                already_shown = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            if (!already_shown) {
                                size_t version_path_len = strlen(name_path) + strlen(version_entry->d_name) + 2;
                                char *version_path = arena_malloc(version_path_len);
                                if (!version_path) continue;
                                snprintf(version_path, version_path_len, "%s/%s", name_path, version_entry->d_name);

                                if (stat(version_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                                    if (!found_any) {
                                        printf("Tracked local-dev packages:\n\n");
                                        found_any = 1;
                                    }

                                    printf("  %s/%s %s\n", author_entry->d_name, name_entry->d_name,
                                           version_entry->d_name);

                                    list_tracking_for_package(tracking_dir, author_entry->d_name,
                                                              name_entry->d_name, version_entry->d_name,
                                                              &connections, &connection_count, &connection_capacity,
                                                              &connection_tracking_enabled);
                                    printf("\n");
                                }
                                arena_free(version_path);
                            }
                        }
                        closedir(version_dir_handle);
                        arena_free(name_path);
                    }
                    closedir(name_dir_handle);
                    arena_free(author_path);
                }
                closedir(author_dir_handle);
            }
        }
    }

    if (tracking_dir) {
        arena_free(tracking_dir);
    }
    if (registry_path) {
        arena_free(registry_path);
    }

    if (connection_tracking_enabled && connection_count > 0) {
        prune_stale_local_dev_connections(connections, connection_count);
    }

    if (!found_any) {
        printf("No local-dev packages are being tracked.\n");
    }

    return 0;
}

/**
 * Clear all local-dev tracking.
 */
static int clear_all_tracking(void) {
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        fprintf(stderr, "Error: Could not determine tracking directory\n");
        return 1;
    }

    struct stat st;
    if (stat(tracking_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("No local-dev tracking to clear.\n");
        arena_free(tracking_dir);
        return 0;
    }

    DIR *dir = opendir(tracking_dir);
    if (dir) {
        struct dirent *author_entry;
        while ((author_entry = readdir(dir)) != NULL) {
            if (author_entry->d_name[0] == '.') continue;

            size_t author_path_len = strlen(tracking_dir) + strlen(author_entry->d_name) + 2;
            char *author_path = arena_malloc(author_path_len);
            if (!author_path) continue;
            snprintf(author_path, author_path_len, "%s/%s", tracking_dir, author_entry->d_name);

            struct stat author_st;
            if (stat(author_path, &author_st) != 0 || !S_ISDIR(author_st.st_mode)) {
                arena_free(author_path);
                continue;
            }

            DIR *author_dir = opendir(author_path);
            if (!author_dir) {
                arena_free(author_path);
                continue;
            }

            struct dirent *name_entry;
            while ((name_entry = readdir(author_dir)) != NULL) {
                if (name_entry->d_name[0] == '.') continue;

                size_t name_path_len = strlen(author_path) + strlen(name_entry->d_name) + 2;
                char *name_path = arena_malloc(name_path_len);
                if (!name_path) continue;
                snprintf(name_path, name_path_len, "%s/%s", author_path, name_entry->d_name);

                struct stat name_st;
                if (stat(name_path, &name_st) != 0 || !S_ISDIR(name_st.st_mode)) {
                    arena_free(name_path);
                    continue;
                }

                DIR *name_dir = opendir(name_path);
                if (!name_dir) {
                    arena_free(name_path);
                    continue;
                }

                struct dirent *version_entry;
                while ((version_entry = readdir(name_dir)) != NULL) {
                    if (version_entry->d_name[0] == '.') continue;

                    size_t version_path_len = strlen(name_path) + strlen(version_entry->d_name) + 2;
                    char *version_path = arena_malloc(version_path_len);
                    if (!version_path) continue;
                    snprintf(version_path, version_path_len, "%s/%s", name_path, version_entry->d_name);

                    struct stat version_st;
                    if (stat(version_path, &version_st) == 0 && S_ISDIR(version_st.st_mode)) {
                        remove_local_dev_from_v1_registry_dat(author_entry->d_name, name_entry->d_name, version_entry->d_name);
                    }

                    arena_free(version_path);
                }

                closedir(name_dir);
                arena_free(name_path);
            }

            closedir(author_dir);
            arena_free(author_path);
        }
        closedir(dir);
    }

    if (remove_directory_recursive_local(tracking_dir)) {
        printf("Cleared all local-dev tracking.\n");
        arena_free(tracking_dir);
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to clear tracking directory\n");
        arena_free(tracking_dir);
        return 1;
    }
}

/**
 * Clear tracking for a specific package.
 */
static int clear_package_tracking(const char *package_name, const char *version) {
    /* Parse author/name */
    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        arena_free(author);
        arena_free(name);
        fprintf(stderr, "Error: Could not determine tracking directory\n");
        return 1;
    }

    /* Build path: tracking_dir/author/name/version */
    size_t path_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *pkg_path = arena_malloc(path_len);
    if (!pkg_path) {
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 1;
    }
    snprintf(pkg_path, path_len, "%s/%s/%s/%s", tracking_dir, author, name, version);

    struct stat st;
    if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("No tracking found for %s/%s %s\n", author, name, version);
        remove_local_dev_from_v1_registry_dat(author, name, version);
        arena_free(pkg_path);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 0;
    }

    if (remove_directory_recursive_local(pkg_path)) {
        printf("Cleared tracking for %s/%s %s\n", author, name, version);
        remove_local_dev_from_v1_registry_dat(author, name, version);
        arena_free(pkg_path);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to clear tracking for %s/%s %s\n", author, name, version);
        arena_free(pkg_path);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 1;
    }
}

/**
 * Simple hash function for path -> filename.
 */
static unsigned long hash_path_local(const char *str) {
    unsigned long hash = DJB2_HASH_INIT;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/**
 * Clear tracking for a specific package and path.
 */
static int clear_path_tracking(const char *package_name, const char *version, const char *path) {
    /* Parse author/name */
    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        arena_free(author);
        arena_free(name);
        fprintf(stderr, "Error: Could not determine tracking directory\n");
        return 1;
    }

    /* Get absolute path */
    char abs_path[PATH_MAX];
    if (!realpath(path, abs_path)) {
        /* Try using the path as-is if it doesn't exist */
        strncpy(abs_path, path, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    /* Create filename from hash of path */
    unsigned long path_hash = hash_path_local(abs_path);
    char hash_filename[32];
    snprintf(hash_filename, sizeof(hash_filename), "%lx", path_hash);

    /* Build path: tracking_dir/author/name/version/hash */
    size_t file_path_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + strlen(hash_filename) + 5;
    char *track_file = arena_malloc(file_path_len);
    if (!track_file) {
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 1;
    }
    snprintf(track_file, file_path_len, "%s/%s/%s/%s/%s", tracking_dir, author, name, version, hash_filename);

    struct stat st;
    if (stat(track_file, &st) != 0) {
        printf("No tracking found for %s in %s/%s %s\n", path, author, name, version);
        arena_free(track_file);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 0;
    }

    if (unlink(track_file) == 0) {
        printf("Cleared tracking for %s in %s/%s %s\n", path, author, name, version);
        arena_free(track_file);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to clear tracking for %s\n", path);
        arena_free(track_file);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return 1;
    }
}

static void prune_stale_local_dev_connections(LocalDevConnection *connections, int connection_count) {
    if (!connections || connection_count == 0) {
        return;
    }

    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        log_error("Failed to initialize rulr for local-dev pruning: %s", err.message);
        return;
    }

    err = rulr_load_rule_file(&rulr, "prune_local_dev_dependencies");
    if (err.is_error) {
        log_error("Failed to load prune_local_dev_dependencies rule: %s", err.message);
        rulr_deinit(&rulr);
        return;
    }

    for (int i = 0; i < connection_count; i++) {
        LocalDevConnection *conn = &connections[i];
        if (!conn->author || !conn->name || !conn->version || !conn->app_path) {
            continue;
        }
        rulr_insert_fact_4s(&rulr, "tracked_connection",
            conn->author, conn->name, conn->version, conn->app_path);
    }

    for (int i = 0; i < connection_count; i++) {
        if (has_processed_app_path(connections, i)) {
            continue;
        }
        insert_app_dependencies_for_path(&rulr, connections[i].app_path);
    }

    err = rulr_evaluate(&rulr);
    if (err.is_error) {
        log_error("Failed to evaluate prune_local_dev_dependencies rule: %s", err.message);
        rulr_deinit(&rulr);
        return;
    }

    EngineRelationView stale_view = rulr_get_relation(&rulr, "stale_connection");
    if (stale_view.pred_id < 0 || stale_view.num_tuples <= 0) {
        rulr_deinit(&rulr);
        return;
    }

    printf("Pruning %d stale local-dev connection%s:\n",
        stale_view.num_tuples, stale_view.num_tuples == 1 ? "" : "s");

    const Tuple *tuples = (const Tuple *)stale_view.tuples;
    for (int i = 0; i < stale_view.num_tuples; i++) {
        const Tuple *t = &tuples[i];
        if (!t || t->arity != 4) {
            continue;
        }
        if (t->fields[0].kind != VAL_SYM || t->fields[1].kind != VAL_SYM ||
            t->fields[2].kind != VAL_SYM || t->fields[3].kind != VAL_SYM) {
            continue;
        }

        const char *author = rulr_lookup_symbol(&rulr, t->fields[0].u.sym);
        const char *name = rulr_lookup_symbol(&rulr, t->fields[1].u.sym);
        const char *version = rulr_lookup_symbol(&rulr, t->fields[2].u.sym);
        const char *app_path = rulr_lookup_symbol(&rulr, t->fields[3].u.sym);
        if (!author || !name || !version || !app_path) {
            continue;
        }

        size_t pkg_name_len = strlen(author) + strlen(name) + 2;
        char *package_name = arena_malloc(pkg_name_len);
        if (!package_name) {
            log_error("Out of memory building package name while pruning local-dev entries");
            continue;
        }
        snprintf(package_name, pkg_name_len, "%s/%s", author, name);
        clear_path_tracking(package_name, version, app_path);
    }

    printf("\n");
    rulr_deinit(&rulr);
}

int cmd_repository_local_dev(int argc, char *argv[]) {
    /* No arguments: list all tracking */
    if (argc <= 1) {
        return list_local_dev_tracking();
    }

    const char *cmd = argv[1];

    if (is_help_flag(cmd)) {
        print_local_dev_usage();
        return 0;
    }

    if (strcmp(cmd, "clear") == 0) {
        for (int i = 2; i < argc; i++) {
            if (is_help_flag(argv[i])) {
                print_local_dev_usage();
                return 0;
            }
        }

        if (argc <= 2) {
            fprintf(stderr, "Error: 'clear' requires --all or a package specifier\n");
            print_local_dev_usage();
            return 1;
        }

        const char *arg = argv[2];

        if (strcmp(arg, "--all") == 0) {
            return clear_all_tracking();
        }

        /* Expect package/name version [path] */
        if (argc < 4) {
            fprintf(stderr, "Error: 'clear' requires a version argument\n");
            print_local_dev_usage();
            return 1;
        }

        const char *package_name = argv[2];
        const char *version = argv[3];

        if (argc >= 5) {
            return clear_path_tracking(package_name, version, argv[4]);
        } else {
            return clear_package_tracking(package_name, version);
        }
    }

    fprintf(stderr, "Error: Unknown local-dev command '%s'\n", cmd);
    print_local_dev_usage();
    return 1;
}

/* ============================================================================
 * Main Entry Point
 * ========================================================================== */

int cmd_repository(int argc, char *argv[]) {
    if (argc < 2) {
        print_repository_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_repository_usage();
        return 0;
    }

    if (strcmp(subcmd, "init") == 0) {
        return cmd_repository_new(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "list") == 0) {
        return cmd_repository_list(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "local-dev") == 0) {
        return cmd_repository_local_dev(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown repository subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s repository --help' for usage information.\n", global_context_program_name());
    return 1;
}
