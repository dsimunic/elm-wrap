#include "package_common.h"
#include "install_local_dev.h"
#include "../../install_env.h"
#include "../../global_context.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include "../../embedded_archive.h"
#include "../../fileutil.h"
#include "../../vendor/cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>
#include <unistd.h>

/* Suppress GCC warn_unused_result for cleanup chdir calls */
#define IGNORE_RESULT(expr) do { if ((expr) != 0) { /* intentionally ignored */ } } while (0)

#define TEMPLATE_PREFIX "templates/package/init"

static void print_package_init_usage(void) {
    printf("Usage: %s package init [OPTIONS] PACKAGE[@VERSION]\n", global_context_program_name());
    printf("\n");
    printf("Initialize a new Elm package.\n");
    printf("\nOptions:\n");
    printf("  --no-local-dev  Skip registering the package in the local-dev registry\n");
    printf("  -y, --yes       Skip confirmation prompt\n");
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

static bool write_elm_json_with_name(
    const char *path,
    const char *package_name,
    const char *package_version,
    void *data,
    size_t size
) {
    char *json_text = arena_malloc(size + 1);
    if (!json_text) {
        fprintf(stderr, "Error: Out of memory while preparing elm.json\n");
        return false;
    }

    memcpy(json_text, data, size);
    json_text[size] = '\0';

    /* Validate JSON structure before modifying */
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

    cJSON *version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!version_item || !cJSON_IsString(version_item)) {
        fprintf(stderr, "Error: Template elm.json is missing a valid \"version\" field\n");
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);

    /* Insert name field after "type": "package" line using string manipulation
     * to preserve the exact formatting of the template */
    const char *type_line_end = strstr(json_text, "\"type\": \"package\",\n");
    if (!type_line_end) {
        fprintf(stderr, "Error: Could not find type field in expected format\n");
        return false;
    }

    /* Find the end of the type line */
    const char *insert_pos = strchr(type_line_end, '\n');
    if (!insert_pos) {
        fprintf(stderr, "Error: Malformed template elm.json\n");
        return false;
    }
    insert_pos++; /* Move past the newline */

    /* Build the name line with proper indentation (4 spaces to match template) */
    char name_line[MAX_TEMP_BUFFER_LENGTH];
    int name_line_len = snprintf(name_line, sizeof(name_line),
                                  "    \"name\": \"%s\",\n", package_name);
    if (name_line_len < 0 || name_line_len >= (int)sizeof(name_line)) {
        fprintf(stderr, "Error: Package name too long\n");
        return false;
    }

    /* Calculate positions and sizes */
    size_t prefix_len = (size_t)(insert_pos - json_text);
    size_t suffix_len = size - prefix_len;
    size_t new_size = size + (size_t)name_line_len;

    char *new_json = arena_malloc(new_size + 1);
    if (!new_json) {
        fprintf(stderr, "Error: Out of memory while preparing elm.json\n");
        return false;
    }

    /* Assemble: prefix + name_line + suffix */
    memcpy(new_json, json_text, prefix_len);
    memcpy(new_json + prefix_len, name_line, (size_t)name_line_len);
    memcpy(new_json + prefix_len + (size_t)name_line_len, insert_pos, suffix_len);
    new_json[new_size] = '\0';

    char *final_json = new_json;
    size_t final_size = new_size;
    if (package_version) {
        const char *version_prefix = "\"version\": \"";
        char *version_pos = strstr(final_json, version_prefix);
        if (!version_pos) {
            fprintf(stderr, "Error: Could not find version field in expected format\n");
            return false;
        }

        char *value_start = version_pos + strlen(version_prefix);
        char *value_end = strchr(value_start, '"');
        if (!value_end) {
            fprintf(stderr, "Error: Malformed version field in template elm.json\n");
            return false;
        }

        size_t prefix2_len = (size_t)(value_start - final_json);
        size_t suffix2_len = final_size - (size_t)(value_end - final_json);
        size_t version_len = strlen(package_version);
        size_t new_size2 = prefix2_len + version_len + suffix2_len;

        char *new_json2 = arena_malloc(new_size2 + 1);
        if (!new_json2) {
            fprintf(stderr, "Error: Out of memory while preparing elm.json\n");
            return false;
        }

        memcpy(new_json2, final_json, prefix2_len);
        memcpy(new_json2 + prefix2_len, package_version, version_len);
        memcpy(new_json2 + prefix2_len + version_len, value_end, suffix2_len);
        new_json2[new_size2] = '\0';

        arena_free(final_json);
        final_json = new_json2;
        final_size = new_size2;
    }

    if (!ensure_parent_directories(path)) {
        fprintf(stderr, "Error: Failed to create parent directories for %s\n", path);
        return false;
    }

    FILE *out = fopen(path, "w");
    if (!out) {
        fprintf(stderr, "Error: Could not open %s for writing\n", path);
        return false;
    }

    size_t written = fwrite(final_json, 1, final_size, out);
    if (written != final_size) {
        fprintf(stderr, "Error: Failed to write %s\n", path);
        fclose(out);
        return false;
    }

    fclose(out);
    return true;
}

static bool extract_templates(const char *package_name, const char *package_version) {
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
            ok = write_elm_json_with_name(target_path, package_name, package_version, data, size);
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

static bool show_init_plan_and_confirm(const char *package_name, const char *resolved_source,
                                       const char *package_version,
                                       bool will_register_local_dev, bool auto_yes) {
    printf("Here is my plan:\n");
    printf("  \n");
    printf("  Create new elm.json for the package:\n");
    if (package_version) {
        printf("    %s    %s\n", package_name, package_version);
    } else {
        printf("    %s    (version from template)\n", package_name);
    }
    printf("  \n");
    printf("  Source: %s\n", resolved_source);
    printf("  \n");

    if (will_register_local_dev) {
        printf("  Also, I will register the package for local development. To prevent that,\n");
        printf("  run this command again and specify --no-local-dev flag.\n");
        printf("\n");
        printf("\n");
        printf("To use this package in an application, run from the application directory:\n");
        printf("    %s package install %s\n", global_context_program_name(), package_name);
        printf("  \n");
    }

    if (!auto_yes) {
        printf("\nWould you like me to proceed? [Y/n] ");
        fflush(stdout);

        char response[MAX_TEMP_BUFFER_LENGTH];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
            printf("Aborted.\n");
            return false;
        }
    }

    return true;
}

int cmd_package_init(int argc, char *argv[]) {
    bool no_local_dev = false;
    bool auto_yes = false;
    const char *package_spec = NULL;
    const char *package_version_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_package_init_usage();
            return 0;
        } else if (strcmp(argv[i], "--no-local-dev") == 0) {
            no_local_dev = true;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            print_package_init_usage();
            return 1;
        } else if (!package_spec) {
            package_spec = argv[i];
        } else if (!package_version_arg) {
            package_version_arg = argv[i];
        } else {
            fprintf(stderr, "Error: Unexpected argument %s\n", argv[i]);
            print_package_init_usage();
            return 1;
        }
    }

    if (!package_spec) {
        fprintf(stderr, "Error: Package name is required\n");
        print_package_init_usage();
        return 1;
    }

    char *author = NULL;
    char *name = NULL;
    Version requested_version = (Version){0};
    bool has_version = false;

    if (strchr(package_spec, '@')) {
        if (package_version_arg) {
            fprintf(stderr, "Error: Version specified twice (use either PACKAGE@VERSION or PACKAGE VERSION)\n");
            print_package_init_usage();
            return 1;
        }
        const char *at = strchr(package_spec, '@');
        if (at == package_spec || at[1] == '\0') {
            fprintf(stderr, "Error: Invalid package specification '%s'\n", package_spec);
            return 1;
        }

        size_t pkg_part_len = (size_t)(at - package_spec);
        char *pkg_part = arena_malloc(pkg_part_len + 1);
        if (!pkg_part) {
            fprintf(stderr, "Error: Out of memory while parsing package name\n");
            return 1;
        }
        strncpy(pkg_part, package_spec, pkg_part_len);
        pkg_part[pkg_part_len] = '\0';

        if (!parse_package_name_init_verbose(pkg_part, &author, &name)) {
            arena_free(pkg_part);
            return 1;
        }
        arena_free(pkg_part);

        if (!version_parse_safe(at + 1, &requested_version)) {
            fprintf(stderr, "Error: Invalid version '%s' (expected X.Y.Z)\n", at + 1);
            arena_free(author);
            arena_free(name);
            return 1;
        }
        has_version = true;
    } else {
        if (!parse_package_name_init_verbose(package_spec, &author, &name)) {
            return 1;
        }
        if (package_version_arg) {
            if (!version_parse_safe(package_version_arg, &requested_version)) {
                fprintf(stderr, "Error: Invalid version '%s' (expected X.Y.Z)\n", package_version_arg);
                arena_free(author);
                arena_free(name);
                return 1;
            }
            has_version = true;
        }
    }

    char package_name_buf[MAX_PACKAGE_NAME_LENGTH];
    int package_name_len = snprintf(package_name_buf, sizeof(package_name_buf), "%s/%s", author, name);
    arena_free(author);
    arena_free(name);
    if (package_name_len < 0 || package_name_len >= (int)sizeof(package_name_buf)) {
        fprintf(stderr, "Error: Package name too long\n");
        return 1;
    }

    char *requested_version_str = NULL;
    if (has_version) {
        requested_version_str = version_to_string(&requested_version);
        if (!requested_version_str) {
            fprintf(stderr, "Error: Out of memory while preparing version\n");
            return 1;
        }
    }

    if (file_exists("elm.json")) {
        fprintf(stderr, "This folder already contains an elm.json.\n");
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }

    if (!embedded_archive_available()) {
        fprintf(stderr, "Error: Embedded templates are not available in this build.\n");
        return 1;
    }

    /* Get current directory for display */
    char cwd[MAX_PATH_LENGTH];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "Error: Failed to get current directory\n");
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }

    /* Show plan and get confirmation */
    if (!show_init_plan_and_confirm(package_name_buf, cwd, requested_version_str, !no_local_dev, auto_yes)) {
        if (requested_version_str) arena_free(requested_version_str);
        return 0;
    }

    if (!extract_templates(package_name_buf, requested_version_str)) {
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }
    if (requested_version_str) arena_free(requested_version_str);

    /* Read actual version from the newly created elm.json */
    char *pkg_author = NULL;
    char *pkg_name = NULL;
    char *pkg_version = NULL;
    if (!read_package_info_from_elm_json("elm.json", &pkg_author, &pkg_name, &pkg_version)) {
        log_error("Failed to read package info from newly created elm.json");
        return 1;
    }

    if (no_local_dev) {
        printf("Successfully created elm.json for %s %s!\n", package_name_buf, pkg_version);
        arena_free(pkg_author);
        arena_free(pkg_name);
        arena_free(pkg_version);
        return 0;
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        arena_free(pkg_author);
        arena_free(pkg_name);
        arena_free(pkg_version);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        arena_free(pkg_author);
        arena_free(pkg_name);
        arena_free(pkg_version);
        return 1;
    }

    int result = register_local_dev_package(".", package_name_buf, env, true, true);
    install_env_free(env);

    if (result == 0) {
        printf("Successfully created and registered %s %s (local)!\n", package_name_buf, pkg_version);
    }

    arena_free(pkg_author);
    arena_free(pkg_name);
    arena_free(pkg_version);

    return result;
}

int package_init_at_path(const char *target_dir, const char *package_spec,
                         bool register_local_dev, bool auto_yes) {
    /* Parse package specification */
    char *author = NULL;
    char *name = NULL;
    Version requested_version = (Version){0};
    bool has_version = false;

    if (strchr(package_spec, '@')) {
        const char *at = strchr(package_spec, '@');
        if (at == package_spec || at[1] == '\0') {
            log_error("Invalid package specification '%s'", package_spec);
            return 1;
        }

        size_t pkg_part_len = (size_t)(at - package_spec);
        char *pkg_part = arena_malloc(pkg_part_len + 1);
        if (!pkg_part) {
            log_error("Out of memory while parsing package name");
            return 1;
        }
        strncpy(pkg_part, package_spec, pkg_part_len);
        pkg_part[pkg_part_len] = '\0';

        if (!parse_package_name_silent(pkg_part, &author, &name)) {
            log_error("Invalid package name: %s", pkg_part);
            arena_free(pkg_part);
            return 1;
        }
        arena_free(pkg_part);

        if (!version_parse_safe(at + 1, &requested_version)) {
            log_error("Invalid version '%s' (expected X.Y.Z)", at + 1);
            arena_free(author);
            arena_free(name);
            return 1;
        }
        has_version = true;
    } else {
        if (!parse_package_name_silent(package_spec, &author, &name)) {
            log_error("Invalid package name: %s", package_spec);
            return 1;
        }
    }

    char package_name_buf[MAX_PACKAGE_NAME_LENGTH];
    int package_name_len = snprintf(package_name_buf, sizeof(package_name_buf), "%s/%s", author, name);
    arena_free(author);
    arena_free(name);
    if (package_name_len < 0 || package_name_len >= (int)sizeof(package_name_buf)) {
        log_error("Package name too long");
        return 1;
    }

    char *requested_version_str = NULL;
    if (has_version) {
        requested_version_str = version_to_string(&requested_version);
        if (!requested_version_str) {
            log_error("Out of memory while preparing version");
            return 1;
        }
    }

    /* Save current directory */
    char original_cwd[MAX_PATH_LENGTH];
    if (!getcwd(original_cwd, sizeof(original_cwd))) {
        log_error("Failed to get current directory");
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }

    /* Change to target directory */
    if (chdir(target_dir) != 0) {
        log_error("Failed to change to directory: %s", target_dir);
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }

    /* Check if elm.json already exists in target */
    if (file_exists("elm.json")) {
        log_error("This folder already contains an elm.json.");
        IGNORE_RESULT(chdir(original_cwd));
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }

    if (!embedded_archive_available()) {
        log_error("Embedded templates are not available in this build.");
        IGNORE_RESULT(chdir(original_cwd));
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }

    /* Extract templates */
    if (!extract_templates(package_name_buf, requested_version_str)) {
        IGNORE_RESULT(chdir(original_cwd));
        if (requested_version_str) arena_free(requested_version_str);
        return 1;
    }
    if (requested_version_str) arena_free(requested_version_str);

    /* Read actual version from newly created elm.json */
    char *pkg_author = NULL;
    char *pkg_name = NULL;
    char *pkg_version = NULL;
    if (!read_package_info_from_elm_json("elm.json", &pkg_author, &pkg_name, &pkg_version)) {
        log_error("Failed to read package info from newly created elm.json");
        IGNORE_RESULT(chdir(original_cwd));
        return 1;
    }

    int result = 0;

    if (register_local_dev) {
        InstallEnv *env = install_env_create();
        if (!env) {
            log_error("Failed to create install environment");
            arena_free(pkg_author);
            arena_free(pkg_name);
            arena_free(pkg_version);
            IGNORE_RESULT(chdir(original_cwd));
            return 1;
        }

        if (!install_env_init(env)) {
            log_error("Failed to initialize install environment");
            install_env_free(env);
            arena_free(pkg_author);
            arena_free(pkg_name);
            arena_free(pkg_version);
            IGNORE_RESULT(chdir(original_cwd));
            return 1;
        }

        result = register_local_dev_package(".", package_name_buf, env, auto_yes, true);
        install_env_free(env);
    }

    arena_free(pkg_author);
    arena_free(pkg_name);
    arena_free(pkg_version);

    /* Return to original directory */
    IGNORE_RESULT(chdir(original_cwd));

    return result;
}
