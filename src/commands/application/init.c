/**
 * init.c - Application init command implementation
 *
 * Initialize a new Elm application project from embedded templates.
 */

#include "application.h"
#include "../../install_env.h"
#include "../../global_context.h"
#include "../../elm_compiler.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../embedded_archive.h"
#include "../../fileutil.h"
#include "../../vendor/cJSON.h"
#include "../review/reporter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>
#include <libgen.h>
#include <errno.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ELM_JSON_PATH "elm.json"
#define DEFAULT_TEMPLATE "application"
#define TEMPLATE_PREFIX "templates/application"
#define ANSI_DULL_CYAN "\033[36m"
#define ANSI_RESET "\033[0m"

static void print_application_init_usage(void) {
    printf("Usage: %s application init [OPTIONS] [TEMPLATE]\n", global_context_program_name());
    printf("\n");
    printf("Initialize a new Elm application project from an embedded template.\n");
    printf("\n");
    printf("Templates:\n");
    printf("  application        Full Browser.application with URL handling (default)\n");
    printf("  document           Browser.document with title and body\n");
    printf("  element            Browser.element for embedding in HTML\n");
    printf("  sandbox            Browser.sandbox for simple programs\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes          Skip confirmation prompt\n");
    printf("  -q, --no-report    Skip printing application info after initialization\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s application init             # Create application template\n", global_context_program_name());
    printf("  %s application init sandbox     # Create sandbox template\n", global_context_program_name());
    printf("  %s application init -y element  # Create element template, no prompt\n", global_context_program_name());
}

static bool is_safe_relative_path(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    if (path[0] == '/') {
        return false;
    }

    const char *segment_start = path;
    const char *p = path;

    while (*p) {
        if (*p == '/') {
            size_t len = (size_t)(p - segment_start);
            if (len == 2 && strncmp(segment_start, "..", 2) == 0) {
                return false;
            }
            segment_start = p + 1;
        }
        p++;
    }

    if (strcmp(segment_start, "..") == 0) {
        return false;
    }

    return true;
}

static bool ensure_directory_exists(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    char *path_copy = arena_strdup(path);
    if (!path_copy) {
        return false;
    }

    bool ok = true;
    char *parent = dirname(path_copy);
    if (parent && strcmp(parent, ".") != 0 && strcmp(parent, "/") != 0) {
        ok = ensure_directory_exists(parent);
    }
    arena_free(path_copy);

    if (!ok) {
        return false;
    }

    if (mkdir(path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
        return false;
    }

    return true;
}

static bool ensure_parent_directories(const char *path) {
    char *path_copy = arena_strdup(path);
    if (!path_copy) {
        return false;
    }

    char *parent = dirname(path_copy);
    bool ok = true;

    if (parent && strcmp(parent, ".") != 0 && strcmp(parent, "/") != 0) {
        ok = ensure_directory_exists(parent);
    }

    arena_free(path_copy);
    return ok;
}

static bool write_file_contents(const char *path, const void *data, size_t size) {
    if (!ensure_parent_directories(path)) {
        fprintf(stderr, "Error: Failed to create parent directories for %s\n", path);
        return false;
    }

    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "Error: Could not open %s for writing\n", path);
        return false;
    }

    size_t written = fwrite(data, 1, size, out);
    fclose(out);
    if (written != size) {
        fprintf(stderr, "Error: Failed to write %s\n", path);
        return false;
    }

    return true;
}

static char *build_template_prefix(const char *compiler_name, const char *compiler_version, const char *template_name) {
    size_t len = strlen(TEMPLATE_PREFIX) + 1 + strlen(compiler_name) + 1 +
                 strlen(compiler_version) + 1 + strlen(template_name) + 1;
    char *prefix = arena_malloc(len);
    if (!prefix) {
        return NULL;
    }
    snprintf(prefix, len, "%s/%s/%s/%s", TEMPLATE_PREFIX, compiler_name, compiler_version, template_name);
    return prefix;
}

static bool extract_template(const char *template_prefix) {
    mz_uint count = embedded_archive_file_count();
    size_t prefix_len = strlen(template_prefix);
    bool found = false;

    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!embedded_archive_file_stat(i, &stat)) {
            continue;
        }

        if (strncmp(stat.m_filename, template_prefix, prefix_len) != 0) {
            continue;
        }

        const char *relative = stat.m_filename + prefix_len;
        if (relative[0] == '/') {
            relative++;
        }

        if (relative[0] == '\0') {
            continue;
        }

        if (!is_safe_relative_path(relative)) {
            fprintf(stderr, "Error: Unsafe template path detected: %s\n", relative);
            return false;
        }

        found = true;

        bool is_dir = embedded_archive_is_directory(i);
        const char *target_path = relative;
        char *clean_path = NULL;

        if (is_dir) {
            clean_path = strip_trailing_slash(relative);
            target_path = clean_path ? clean_path : relative;
        }

        if (is_dir) {
            if (!ensure_directory_exists(target_path)) {
                fprintf(stderr, "Error: Failed to create directory %s\n", target_path);
                arena_free(clean_path);
                return false;
            }
            arena_free(clean_path);
            continue;
        }

        void *data = NULL;
        size_t size = 0;
        if (!embedded_archive_extract(stat.m_filename, &data, &size)) {
            fprintf(stderr, "Error: Failed to extract %s from embedded templates\n", stat.m_filename);
            arena_free(clean_path);
            return false;
        }

        bool ok = write_file_contents(target_path, data, size);

        arena_free(data);
        arena_free(clean_path);

        if (!ok) {
            return false;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: No embedded templates found at %s\n", template_prefix);
        return false;
    }

    return true;
}

static bool template_exists(const char *compiler_name, const char *compiler_version, const char *template_name) {
    char *prefix = build_template_prefix(compiler_name, compiler_version, template_name);
    if (!prefix) {
        return false;
    }

    mz_uint count = embedded_archive_file_count();
    size_t prefix_len = strlen(prefix);
    bool found = false;

    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!embedded_archive_file_stat(i, &stat)) {
            continue;
        }

        if (strncmp(stat.m_filename, prefix, prefix_len) == 0) {
            found = true;
            break;
        }
    }

    arena_free(prefix);
    return found;
}

static int collect_template_files(const char *template_prefix, const char ***out_paths) {
    mz_uint count = embedded_archive_file_count();
    size_t prefix_len = strlen(template_prefix);

    int capacity = 16;
    int file_count = 0;
    const char **paths = arena_malloc(capacity * sizeof(char *));

    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!embedded_archive_file_stat(i, &stat)) {
            continue;
        }

        if (strncmp(stat.m_filename, template_prefix, prefix_len) != 0) {
            continue;
        }

        const char *relative = stat.m_filename + prefix_len;
        if (relative[0] == '/') {
            relative++;
        }

        if (relative[0] == '\0') {
            continue;
        }

        /* Skip directories (they end with /) */
        size_t rel_len = strlen(relative);
        if (rel_len > 0 && relative[rel_len - 1] == '/') {
            continue;
        }

        if (file_count >= capacity) {
            capacity *= 2;
            paths = arena_realloc(paths, capacity * sizeof(char *));
        }
        paths[file_count++] = arena_strdup(relative);
    }

    *out_paths = paths;
    return file_count;
}

static bool show_init_plan_and_confirm(const char *template_name, const char *template_prefix, const char *cwd, bool auto_yes) {
    printf("Here is my plan:\n");
    printf("  \n");
    printf("  Create a new Elm application using the '%s' template.\n", template_name);
    printf("  \n");
    printf("  Location: %s\n", cwd);
    printf("  \n");
    printf("  Files to create:\n");

    const char **paths = NULL;
    int file_count = collect_template_files(template_prefix, &paths);

    if (file_count > 0) {
        ReporterConfig cfg = reporter_default_config();
        cfg.base_path = NULL;
        reporter_print_file_tree(&cfg, paths, file_count);
    } else {
        printf("    (no files found in template)\n");
    }

    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to proceed? [Y/n]: ");
        fflush(stdout);

        char response[10];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
            printf("Aborted.\n");
            return false;
        }
    }

    return true;
}

int cmd_application_init(int argc, char *argv[]) {
    bool skip_prompt = false;
    bool no_report = false;
    const char *template_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_application_init_usage();
            return 0;
        } else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            skip_prompt = true;
        } else if (strcmp(argv[i], "--no-report") == 0 || strcmp(argv[i], "-q") == 0) {
            no_report = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            print_application_init_usage();
            return 1;
        } else if (!template_name) {
            template_name = argv[i];
        } else {
            fprintf(stderr, "Error: Unexpected argument %s\n", argv[i]);
            print_application_init_usage();
            return 1;
        }
    }

    if (!template_name) {
        template_name = DEFAULT_TEMPLATE;
    }

    if (file_exists(ELM_JSON_PATH)) {
        fprintf(stderr, "%s-- EXISTING PROJECT ------------------------------------------------------------\n", ANSI_DULL_CYAN);
        fprintf(stderr, "\n");
        fprintf(stderr, "You already have an elm.json file, so there is nothing for me to initialize!\n");
        fprintf(stderr, "\n%s", ANSI_RESET);
        return 1;
    }

    if (!embedded_archive_available()) {
        fprintf(stderr, "Error: Embedded templates are not available in this build.\n");
        return 1;
    }

    GlobalContext *ctx = global_context_get();
    const char *compiler_name = global_context_compiler_name();
    const char *compiler_version = ctx && ctx->compiler_version ? ctx->compiler_version : NULL;

    if (!compiler_version) {
        compiler_version = elm_compiler_get_version();
        if (!compiler_version) {
            fprintf(stderr, "Error: Could not determine compiler version.\n");
            fprintf(stderr, "Make sure the Elm compiler is installed and in your PATH.\n");
            return 1;
        }
    }

    if (!template_exists(compiler_name, compiler_version, template_name)) {
        fprintf(stderr, "Error: Template '%s' not found for %s %s.\n",
                template_name, compiler_name, compiler_version);
        fprintf(stderr, "\n");
        fprintf(stderr, "Available templates can be listed with:\n");
        fprintf(stderr, "  %s application list-templates\n", global_context_program_name());
        return 1;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "Error: Failed to get current directory\n");
        return 1;
    }

    char *template_prefix = build_template_prefix(compiler_name, compiler_version, template_name);
    if (!template_prefix) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }

    if (!skip_prompt) {
        if (!show_init_plan_and_confirm(template_name, template_prefix, cwd, false)) {
            arena_free(template_prefix);
            return 0;
        }
    }

    if (!extract_template(template_prefix)) {
        arena_free(template_prefix);
        return 1;
    }

    arena_free(template_prefix);

    printf("Successfully created %s application using '%s' template!\n", compiler_name, template_name);

    if (!no_report) {
        printf("\n");
        char *info_argv[] = { argv[0], NULL };
        cmd_application_info(1, info_argv);
    }

    return 0;
}

int cmd_application_list_templates(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s application list-templates\n", global_context_program_name());
            printf("\n");
            printf("List available application templates for the current compiler.\n");
            return 0;
        }
    }

    if (!embedded_archive_available()) {
        fprintf(stderr, "Error: Embedded templates are not available in this build.\n");
        return 1;
    }

    GlobalContext *ctx = global_context_get();
    const char *compiler_name = global_context_compiler_name();
    const char *compiler_version = ctx && ctx->compiler_version ? ctx->compiler_version : NULL;

    if (!compiler_version) {
        compiler_version = elm_compiler_get_version();
        if (!compiler_version) {
            fprintf(stderr, "Error: Could not determine compiler version.\n");
            fprintf(stderr, "Make sure the Elm compiler is installed and in your PATH.\n");
            return 1;
        }
    }

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/%s/%s/", TEMPLATE_PREFIX, compiler_name, compiler_version);
    size_t prefix_len = strlen(prefix);

    printf("Available templates for %s %s:\n\n", compiler_name, compiler_version);

    mz_uint count = embedded_archive_file_count();
    char *found_templates[32];
    int template_count = 0;

    for (mz_uint i = 0; i < count && template_count < 32; i++) {
        mz_zip_archive_file_stat stat;
        if (!embedded_archive_file_stat(i, &stat)) {
            continue;
        }

        if (strncmp(stat.m_filename, prefix, prefix_len) != 0) {
            continue;
        }

        const char *relative = stat.m_filename + prefix_len;
        if (relative[0] == '\0') {
            continue;
        }

        const char *slash = strchr(relative, '/');
        size_t name_len = slash ? (size_t)(slash - relative) : strlen(relative);

        if (name_len == 0) {
            continue;
        }

        char *name = arena_malloc(name_len + 1);
        if (!name) {
            continue;
        }
        strncpy(name, relative, name_len);
        name[name_len] = '\0';

        bool duplicate = false;
        for (int j = 0; j < template_count; j++) {
            if (strcmp(found_templates[j], name) == 0) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            found_templates[template_count++] = name;
        } else {
            arena_free(name);
        }
    }

    if (template_count == 0) {
        printf("  (no templates found)\n");
    } else {
        for (int i = 0; i < template_count; i++) {
            const char *desc = "";
            if (strcmp(found_templates[i], "application") == 0) {
                desc = "Full Browser.application with URL handling";
            } else if (strcmp(found_templates[i], "document") == 0) {
                desc = "Browser.document with title and body";
            } else if (strcmp(found_templates[i], "element") == 0) {
                desc = "Browser.element for embedding in HTML";
            } else if (strcmp(found_templates[i], "sandbox") == 0) {
                desc = "Browser.sandbox for simple programs";
            } else if (strcmp(found_templates[i], "worker") == 0) {
                desc = "Platform.worker for background processing";
            }
            printf("  %-15s %s\n", found_templates[i], desc);
            
            arena_free(found_templates[i]);
        }
        printf("\n");
    }

    return 0;
}
