/**
 * repository.c - Repository command group for managing local package repositories
 *
 * This command provides utilities for creating and listing local package
 * repository directories organized by compiler name and version.
 */

#include "repository.h"
#include "../../alloc.h"
#include "../../progname.h"
#include "../../env_defaults.h"
#include "../../elm_compiler.h"
#include "../../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ============================================================================
 * Usage
 * ========================================================================== */

static void print_repository_usage(void) {
    printf("Usage: %s repository SUBCOMMAND [OPTIONS]\n", program_name);
    printf("\n");
    printf("Manage local package repositories.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  new [<root_path>]     Create a new repository directory\n");
    printf("  list [<root_path>]    List repositories at path\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
}

static void print_new_usage(void) {
    printf("Usage: %s repository new [<root_path>] [OPTIONS]\n", program_name);
    printf("\n");
    printf("Create a new repository directory for the current (or specified) compiler.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <root_path>           Root path for repositories (default: ELM_WRAP_REPOSITORY_LOCAL_PATH)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --compiler <name>     Compiler name (elm, lamdera, wrapc, etc.)\n");
    printf("  --version <version>   Compiler version (e.g., 0.19.1)\n");
    printf("  -h, --help            Show this help message\n");
    printf("\n");
    printf("The repository path is: <root_path>/<compiler>/<version>/\n");
    printf("For example: ~/.elm-wrap/repository/elm/0.19.1/\n");
}

static void print_list_usage(void) {
    printf("Usage: %s repository list [<root_path>]\n", program_name);
    printf("\n");
    printf("List all repositories at the given path.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <root_path>           Root path for repositories (default: ELM_WRAP_REPOSITORY_LOCAL_PATH)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
}

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

/**
 * Get the compiler name from the compiler path.
 * Extracts the basename of the compiler path.
 * Returns "elm" if no custom path is set.
 */
static char *get_compiler_name(void) {
    const char *compiler_path = getenv("ELM_WRAP_ELM_COMPILER_PATH");
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
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
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
                strcpy(effective_root, home);
                strcat(effective_root, root_path + 1);
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
        fprintf(stderr, "Set ELM_WRAP_REPOSITORY_LOCAL_PATH or provide a path argument\n");
        return 1;
    }

    /* Get compiler name (from argument, ELM_WRAP_ELM_COMPILER_PATH basename, or "elm") */
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

    /* Create the directory */
    log_debug("Creating repository at: %s", repo_path);

    if (mkdir_p(repo_path) != 0) {
        fprintf(stderr, "Error: Failed to create directory '%s': %s\n", repo_path, strerror(errno));
        return 1;
    }

    printf("Created repository: %s\n", repo_path);
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
                strcpy(effective_root, home);
                strcat(effective_root, root_path + 1);
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
        fprintf(stderr, "Set ELM_WRAP_REPOSITORY_LOCAL_PATH or provide a path argument\n");
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
        /* Skip . and .. */
        if (compiler_entry->d_name[0] == '.') {
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

    if (strcmp(subcmd, "new") == 0) {
        return cmd_repository_new(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "list") == 0) {
        return cmd_repository_list(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown repository subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s repository --help' for usage information.\n", program_name);
    return 1;
}
