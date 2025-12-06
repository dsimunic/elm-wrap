#include "package_common.h"
#include "install_local_dev.h"
#include "../../install_env.h"
#include "../../global_context.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../embedded_archive.h"
#include "../../fileutil.h"
#include "../../vendor/cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEMPLATE_PREFIX "templates/package/init"

static void print_package_init_usage(void) {
    printf("Usage: %s package init [--no-local-dev] PACKAGE\n", global_context_program_name());
    printf("\n");
    printf("Initialize a new Elm package from embedded templates.\n");
    printf("\nOptions:\n");
    printf("  --no-local-dev  Skip registering the package in the local-dev registry\n");
    printf("  -h, --help      Show this help message\n");
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

static cJSON* rebuild_with_name(cJSON *root, const char *package_name) {
    if (!root || !cJSON_IsObject(root) || !package_name) {
        return NULL;
    }

    cJSON *type_item = cJSON_DetachItemFromObjectCaseSensitive(root, "type");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "name");

    cJSON *new_root = cJSON_CreateObject();
    if (!new_root) {
        if (type_item) {
            cJSON_AddItemToObject(root, "type", type_item);
        }
        return NULL;
    }

    if (type_item) {
        cJSON_AddItemToObject(new_root, "type", type_item);
    }

    cJSON *name_item = cJSON_CreateString(package_name);
    if (!name_item) {
        cJSON_Delete(new_root);
        if (type_item) {
            cJSON_Delete(type_item);
        }
        return NULL;
    }
    cJSON_AddItemToObject(new_root, "name", name_item);

    while (root->child) {
        cJSON *detached = cJSON_DetachItemFromObjectCaseSensitive(root, root->child->string);
        if (detached) {
            cJSON_AddItemToObject(new_root, detached->string, detached);
        }
    }

    cJSON_Delete(root);
    return new_root;
}

static bool write_elm_json_with_name(const char *path, const char *package_name, void *data, size_t size) {
    char *json_text = arena_malloc(size + 1);
    if (!json_text) {
        fprintf(stderr, "Error: Out of memory while preparing elm.json\n");
        return false;
    }

    memcpy(json_text, data, size);
    json_text[size] = '\0';

    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        fprintf(stderr, "Error: Failed to parse template elm.json\n");
        return false;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        fprintf(stderr, "Error: Template elm.json is missing a valid \"type\" field\n");
        cJSON_Delete(root);
        return false;
    }

    if (strcmp(type_item->valuestring, "package") != 0) {
        fprintf(stderr, "Error: Template elm.json must be a package project\n");
        cJSON_Delete(root);
        return false;
    }

    cJSON *updated = rebuild_with_name(root, package_name);
    if (!updated) {
        fprintf(stderr, "Error: Failed to inject package name into elm.json\n");
        cJSON_Delete(root);
        return false;
    }

    char *printed = cJSON_PrintBuffered(updated, 256, 1);
    if (!printed) {
        fprintf(stderr, "Error: Failed to serialize elm.json\n");
        cJSON_Delete(updated);
        return false;
    }

    size_t printed_len = strlen(printed);

    if (!ensure_parent_directories(path)) {
        fprintf(stderr, "Error: Failed to create parent directories for %s\n", path);
        cJSON_free(printed);
        cJSON_Delete(updated);
        return false;
    }

    FILE *out = fopen(path, "w");
    if (!out) {
        fprintf(stderr, "Error: Could not open %s for writing\n", path);
        cJSON_free(printed);
        cJSON_Delete(updated);
        return false;
    }

    size_t written = fwrite(printed, 1, printed_len, out);
    if (written != printed_len) {
        fprintf(stderr, "Error: Failed to write %s\n", path);
        fclose(out);
        cJSON_free(printed);
        cJSON_Delete(updated);
        return false;
    }

    if (printed_len == 0 || printed[printed_len - 1] != '\n') {
        fputc('\n', out);
    }
    fclose(out);

    cJSON_free(printed);
    cJSON_Delete(updated);
    return true;
}

static bool extract_templates(const char *package_name) {
    mz_uint count = embedded_archive_file_count();
    size_t prefix_len = strlen(TEMPLATE_PREFIX);
    bool found = false;

    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!embedded_archive_file_stat(i, &stat)) {
            continue;
        }

        if (strncmp(stat.m_filename, TEMPLATE_PREFIX, prefix_len) != 0) {
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

        bool ok;
        if (strcmp(target_path, "elm.json") == 0) {
            ok = write_elm_json_with_name(target_path, package_name, data, size);
        } else {
            ok = write_file_contents(target_path, data, size);
        }

        arena_free(data);
        arena_free(clean_path);

        if (!ok) {
            return false;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: No embedded templates found at %s\n", TEMPLATE_PREFIX);
        return false;
    }

    return true;
}

int cmd_package_init(int argc, char *argv[]) {
    bool no_local_dev = false;
    const char *package_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_package_init_usage();
            return 0;
        } else if (strcmp(argv[i], "--no-local-dev") == 0) {
            no_local_dev = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            print_package_init_usage();
            return 1;
        } else if (!package_name) {
            package_name = argv[i];
        } else {
            fprintf(stderr, "Error: Unexpected argument %s\n", argv[i]);
            print_package_init_usage();
            return 1;
        }
    }

    if (!package_name) {
        fprintf(stderr, "Error: Package name is required\n");
        print_package_init_usage();
        return 1;
    }

    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    if (author[0] == '\0' || name[0] == '\0') {
        fprintf(stderr, "Error: Package name must be in the form author/name\n");
        arena_free(author);
        arena_free(name);
        return 1;
    }

    arena_free(author);
    arena_free(name);

    if (file_exists("elm.json")) {
        fprintf(stderr, "This folder already contains an elm.json.\n");
        return 1;
    }

    if (!embedded_archive_available()) {
        fprintf(stderr, "Error: Embedded templates are not available in this build.\n");
        return 1;
    }

    if (!extract_templates(package_name)) {
        return 1;
    }

    if (no_local_dev) {
        return 0;
    }

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

    int result = register_local_dev_package(".", package_name, env, true);
    install_env_free(env);

    return result;
}
