#include "extract_cmd.h"
#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../install_env.h"
#include "../../global_context.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include "../../fileutil.h"
#include "../../elm_json.h"
#include "../../elm_project.h"
#include "../../ast/skeleton.h"
#include "../../vendor/cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

/* ============================================================================
 * Data structures
 * ========================================================================== */

typedef struct {
    char **abs_paths;       /* Absolute source paths */
    char **dest_relatives;  /* Destination paths relative to target/src */
    int count;
    int capacity;
} SelectedFiles;

typedef struct {
    char *importing_file_abs;
    char *importing_module_name;
    char *imported_module_name;
    char *imported_file_abs;
} ExtractViolation;

typedef struct {
    ExtractViolation *violations;
    int count;
    int capacity;
} ViolationList;

/* ============================================================================
 * Usage and helpers
 * ========================================================================== */

static void print_extract_usage(void) {
    printf("Usage: %s package extract PACKAGE TARGET_PATH PATH [PATH...]\n",
           global_context_program_name());
    printf("\n");
    printf("Extract Elm source from an application into a new package.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PACKAGE       Package name (author/name or author/name@version)\n");
    printf("  TARGET_PATH   Directory where new package will be created\n");
    printf("  PATH          One or more source files or directories to extract\n");
    printf("\n");
    printf("Multiple paths can be specified to extract both a head module and its\n");
    printf("submodule directory. For example:\n");
    printf("  %s package extract me/pkg ../pkg src/Foo.elm src/Foo\n",
           global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help    Show this help message\n");
}

static bool path_is_elm_file(const char *path) {
    size_t len = strlen(path);
    return len > 4 && strcmp(path + len - 4, ".elm") == 0;
}

static bool path_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool path_exists_stat(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char *get_realpath_safe(const char *path) {
    char resolved[MAX_PATH_LENGTH];
    if (!realpath(path, resolved)) {
        return NULL;
    }
    return arena_strdup(resolved);
}

static char *compute_relative_path(const char *base_abs, const char *file_abs) {
    /* Both must be absolute paths */
    size_t base_len = strlen(base_abs);
    size_t file_len = strlen(file_abs);

    /* file_abs must start with base_abs + "/" */
    if (file_len <= base_len || strncmp(file_abs, base_abs, base_len) != 0) {
        return NULL;
    }

    /* Skip past base_abs and the separator */
    const char *rel = file_abs + base_len;
    while (*rel == '/') {
        rel++;
    }

    return arena_strdup(rel);
}

/* ============================================================================
 * File selection
 * ========================================================================== */

static void selected_files_init(SelectedFiles *sf) {
    sf->capacity = INITIAL_FILE_CAPACITY;
    sf->count = 0;
    sf->abs_paths = arena_malloc(sf->capacity * sizeof(char*));
    sf->dest_relatives = arena_malloc(sf->capacity * sizeof(char*));
}

static void selected_files_add(SelectedFiles *sf, const char *abs_path,
                               const char *dest_relative) {
    if (sf->count >= sf->capacity) {
        sf->capacity *= 2;
        sf->abs_paths = arena_realloc(sf->abs_paths, sf->capacity * sizeof(char*));
        sf->dest_relatives = arena_realloc(sf->dest_relatives, sf->capacity * sizeof(char*));
    }
    sf->abs_paths[sf->count] = arena_strdup(abs_path);
    sf->dest_relatives[sf->count] = arena_strdup(dest_relative);
    sf->count++;
}

static bool selected_files_contains(const SelectedFiles *sf, const char *abs_path) {
    for (int i = 0; i < sf->count; i++) {
        if (strcmp(sf->abs_paths[i], abs_path) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Collect files recursively from a directory.
 *
 * @param dir_abs        Absolute path of current directory being scanned
 * @param root_abs       Absolute path of the top-level source directory
 * @param dir_basename   Basename of the source directory (for destination paths)
 * @param out            Output: files are added here
 */
static void collect_files_recursive(const char *dir_abs, const char *root_abs,
                                    const char *dir_basename, SelectedFiles *out) {
    DIR *dir = opendir(dir_abs);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[MAX_PATH_LENGTH];
        int len = snprintf(child_path, sizeof(child_path), "%s/%s", dir_abs, entry->d_name);
        if (len < 0 || len >= (int)sizeof(child_path)) {
            continue;
        }

        struct stat st;
        if (stat(child_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_files_recursive(child_path, root_abs, dir_basename, out);
        } else if (S_ISREG(st.st_mode)) {
            char *abs = get_realpath_safe(child_path);
            if (abs) {
                /* Compute destination: dir_basename + "/" + relative_from_root */
                char *rel_from_root = compute_relative_path(root_abs, abs);
                if (rel_from_root) {
                    char dest_rel[MAX_PATH_LENGTH];
                    int dest_len = snprintf(dest_rel, sizeof(dest_rel), "%s/%s",
                                           dir_basename, rel_from_root);
                    arena_free(rel_from_root);

                    if (dest_len >= 0 && dest_len < (int)sizeof(dest_rel)) {
                        selected_files_add(out, abs, dest_rel);
                    }
                }
                arena_free(abs);
            }
        }
    }

    closedir(dir);
}

/* ============================================================================
 * Violation tracking
 * ========================================================================== */

static void violation_list_init(ViolationList *vl) {
    vl->capacity = INITIAL_SMALL_CAPACITY;
    vl->count = 0;
    vl->violations = arena_malloc(vl->capacity * sizeof(ExtractViolation));
}

static void violation_list_add(ViolationList *vl, const char *importing_file,
                                const char *importing_mod, const char *imported_mod,
                                const char *imported_file) {
    if (vl->count >= vl->capacity) {
        vl->capacity *= 2;
        vl->violations = arena_realloc(vl->violations,
                                       vl->capacity * sizeof(ExtractViolation));
    }

    ExtractViolation *v = &vl->violations[vl->count++];
    v->importing_file_abs = arena_strdup(importing_file);
    v->importing_module_name = arena_strdup(importing_mod);
    v->imported_module_name = arena_strdup(imported_mod);
    v->imported_file_abs = arena_strdup(imported_file);
}

/* ============================================================================
 * Source directory resolution
 * ========================================================================== */

static char **get_app_source_dirs_abs(const char *elm_json_path, int *out_count) {
    char **source_dirs = elm_parse_source_directories(elm_json_path, out_count);
    if (!source_dirs || *out_count == 0) {
        /* Default to "src" if not specified */
        *out_count = 1;
        source_dirs = arena_malloc(sizeof(char*));
        source_dirs[0] = arena_strdup("src");
    }

    /* Get current working directory */
    char cwd[MAX_PATH_LENGTH];
    if (!getcwd(cwd, sizeof(cwd))) {
        return NULL;
    }

    /* Normalize each to absolute path */
    for (int i = 0; i < *out_count; i++) {
        char full_path[MAX_PATH_LENGTH];
        int len = snprintf(full_path, sizeof(full_path), "%s/%s", cwd, source_dirs[i]);
        if (len < 0 || len >= (int)sizeof(full_path)) {
            return NULL;
        }

        char *abs = get_realpath_safe(full_path);
        if (!abs) {
            /* If realpath fails, source dir doesn't exist - skip it */
            continue;
        }

        arena_free(source_dirs[i]);
        source_dirs[i] = abs;
    }

    return source_dirs;
}

/* ============================================================================
 * Import resolution
 * ========================================================================== */

static char *resolve_local_import_to_file(const char *module_name,
                                          char **srcdirs_abs, int srcdir_count) {
    for (int i = 0; i < srcdir_count; i++) {
        char *candidate = elm_module_name_to_path(module_name, srcdirs_abs[i]);
        if (file_exists(candidate)) {
            char *abs = get_realpath_safe(candidate);
            arena_free(candidate);
            return abs;
        }
        arena_free(candidate);
    }
    return NULL;
}

/* ============================================================================
 * Directory creation
 * ========================================================================== */

static bool ensure_directory_exists_recursive(const char *path) {
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

    char *parent = dirname(path_copy);
    if (parent && strcmp(parent, ".") != 0 && strcmp(parent, "/") != 0) {
        if (!ensure_directory_exists_recursive(parent)) {
            arena_free(path_copy);
            return false;
        }
    }
    arena_free(path_copy);

    if (mkdir(path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
        return false;
    }

    return true;
}

/* ============================================================================
 * File moving
 * ========================================================================== */

static bool move_file(const char *src, const char *dest) {
    /* Create parent directories by iterating through path components */
    char *dest_copy = arena_strdup(dest);
    if (!dest_copy) {
        log_error("Out of memory");
        return false;
    }

    /* Get the parent directory */
    char *last_slash = strrchr(dest_copy, '/');
    if (last_slash) {
        *last_slash = '\0';  /* Temporarily null-terminate to get parent */

        /* Create all parent directories */
        char temp_path[MAX_PATH_LENGTH];
        temp_path[0] = '\0';

        char *component = dest_copy;
        if (dest_copy[0] == '/') {
            strcpy(temp_path, "/");
            component = dest_copy + 1;
        }

        char *slash;
        while ((slash = strchr(component, '/')) != NULL) {
            *slash = '\0';

            if (temp_path[0] == '/' && temp_path[1] == '\0') {
                snprintf(temp_path, sizeof(temp_path), "/%s", component);
            } else if (temp_path[0] != '\0') {
                size_t len = strlen(temp_path);
                snprintf(temp_path + len, sizeof(temp_path) - len, "/%s", component);
            } else {
                snprintf(temp_path, sizeof(temp_path), "%s", component);
            }

            struct stat st;
            if (stat(temp_path, &st) != 0) {
                if (mkdir(temp_path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
                    log_error("Failed to create directory %s: %s", temp_path, strerror(errno));
                    arena_free(dest_copy);
                    return false;
                }
            }

            *slash = '/';
            component = slash + 1;
        }

        /* Create the final parent component */
        if (*component) {
            if (temp_path[0] == '/' && temp_path[1] == '\0') {
                snprintf(temp_path, sizeof(temp_path), "/%s", component);
            } else if (temp_path[0] != '\0') {
                size_t len = strlen(temp_path);
                snprintf(temp_path + len, sizeof(temp_path) - len, "/%s", component);
            } else {
                snprintf(temp_path, sizeof(temp_path), "%s", component);
            }

            struct stat st;
            if (stat(temp_path, &st) != 0) {
                if (mkdir(temp_path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
                    log_error("Failed to create directory %s: %s", temp_path, strerror(errno));
                    arena_free(dest_copy);
                    return false;
                }
            }
        }
    }
    arena_free(dest_copy);

    /* Try rename first */
    if (rename(src, dest) == 0) {
        return true;
    }

    /* If EXDEV (cross-device), fallback to copy + unlink */
    if (errno == EXDEV) {
        size_t size = 0;
        char *content = file_read_contents_bounded(src, MAX_ELM_SOURCE_FILE_BYTES, &size);
        if (!content) {
            log_error("Failed to read source file %s for copying", src);
            return false;
        }

        FILE *out = fopen(dest, "wb");
        if (!out) {
            log_error("Failed to open destination file %s for writing: %s",
                     dest, strerror(errno));
            arena_free(content);
            return false;
        }

        size_t written = fwrite(content, 1, size, out);
        fclose(out);
        arena_free(content);

        if (written != size) {
            log_error("Failed to write complete file to %s", dest);
            return false;
        }

        if (unlink(src) != 0) {
            log_error("Failed to remove source file %s after copy: %s",
                     src, strerror(errno));
            return false;
        }

        return true;
    }

    log_error("Failed to rename %s to %s: %s", src, dest, strerror(errno));
    return false;
}

/* ============================================================================
 * Main command implementation
 * ========================================================================== */

int cmd_extract(int argc, char *argv[]) {
    /* Phase A: Parse arguments */
    if (argc < 4) {
        if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
            print_extract_usage();
            return 0;
        }
        log_error("Insufficient arguments");
        print_extract_usage();
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_extract_usage();
        return 0;
    }

    const char *package_spec = argv[1];
    const char *target_path = argv[2];

    /* Phase B: Validate application project */
    ElmJson *app_json = elm_json_read("elm.json");
    if (!app_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        return 1;
    }

    if (app_json->type != ELM_PROJECT_APPLICATION) {
        log_error("This command must be run in an Elm application project (elm.json type=\"application\").");
        elm_json_free(app_json);
        return 1;
    }

    /* Phase C: Parse package specification */
    char *author = NULL;
    char *name = NULL;
    Version version = {0};
    bool has_version = false;

    if (!parse_package_with_version(package_spec, &author, &name, &version)) {
        if (!parse_package_name(package_spec, &author, &name)) {
            log_error("Invalid package specification: %s", package_spec);
            log_error("Expected format: author/name or author/name@version");
            elm_json_free(app_json);
            return 1;
        }
    } else {
        has_version = true;
    }

    char package_name[MAX_PACKAGE_NAME_LENGTH];
    int pkg_len = snprintf(package_name, sizeof(package_name), "%s/%s", author, name);
    if (pkg_len < 0 || pkg_len >= (int)sizeof(package_name)) {
        log_error("Package name too long");
        arena_free(author);
        arena_free(name);
        elm_json_free(app_json);
        return 1;
    }

    char *version_str = NULL;
    if (has_version) {
        version_str = version_to_string(&version);
    }

    arena_free(author);
    arena_free(name);

    /* Phase D: Validate paths */
    if (path_exists_stat(target_path)) {
        log_error("Target path already exists: %s", target_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    /* Collect all source paths from argv[3..argc-1] */
    int source_path_count = argc - 3;
    const char **source_paths = (const char **)&argv[3];

    /* Validate each source path */
    for (int i = 0; i < source_path_count; i++) {
        const char *src_path = source_paths[i];

        if (!path_exists_stat(src_path)) {
            log_error("Path does not exist: %s", src_path);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }

        bool is_file = !path_is_directory(src_path);
        if (is_file && !path_is_elm_file(src_path)) {
            log_error("PATH must be an .elm file or a directory: %s", src_path);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }
    }

    /* Phase E: Enumerate selected files from all source paths */
    SelectedFiles selected;
    selected_files_init(&selected);

    for (int i = 0; i < source_path_count; i++) {
        const char *src_path = source_paths[i];
        bool is_file = !path_is_directory(src_path);

        if (is_file) {
            /* Single file: destination is just the basename */
            char *abs = get_realpath_safe(src_path);
            if (!abs) {
                log_error("Failed to resolve path: %s", src_path);
                if (version_str) arena_free(version_str);
                elm_json_free(app_json);
                return 1;
            }

            char *abs_copy = arena_strdup(abs);
            char *file_basename = basename(abs_copy);
            selected_files_add(&selected, abs, file_basename);
            arena_free(abs_copy);
            arena_free(abs);
        } else {
            /* Directory: files go under dir_basename/relative_path */
            char *source_abs = get_realpath_safe(src_path);
            if (!source_abs) {
                log_error("Failed to resolve path: %s", src_path);
                if (version_str) arena_free(version_str);
                elm_json_free(app_json);
                return 1;
            }

            char *source_abs_copy = arena_strdup(source_abs);
            char *dir_basename = arena_strdup(basename(source_abs_copy));
            arena_free(source_abs_copy);

            collect_files_recursive(source_abs, source_abs, dir_basename, &selected);
            arena_free(dir_basename);
            arena_free(source_abs);
        }
    }

    /* Count .elm files */
    int elm_file_count = 0;
    for (int i = 0; i < selected.count; i++) {
        if (path_is_elm_file(selected.abs_paths[i])) {
            elm_file_count++;
        }
    }

    if (elm_file_count == 0) {
        log_error("No .elm files found in specified paths");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    /* Phase F: Out-of-selection import validation */
    int srcdir_count = 0;
    char **srcdirs_abs = get_app_source_dirs_abs("elm.json", &srcdir_count);
    if (!srcdirs_abs) {
        log_error("Failed to parse source directories from elm.json");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    ViolationList violations;
    violation_list_init(&violations);

    for (int i = 0; i < selected.count; i++) {
        const char *file_path = selected.abs_paths[i];
        if (!path_is_elm_file(file_path)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(file_path);
        if (!module) {
            log_error("Failed to parse Elm module: %s", file_path);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }

        for (int j = 0; j < module->imports_count; j++) {
            const char *import_name = module->imports[j].module_name;

            char *resolved = resolve_local_import_to_file(import_name,
                                                         srcdirs_abs, srcdir_count);
            if (resolved) {
                /* This is a local project import */
                if (!selected_files_contains(&selected, resolved)) {
                    /* Violation: imports a local module outside selection */
                    violation_list_add(&violations, file_path, module->module_name,
                                      import_name, resolved);
                }
                arena_free(resolved);
            }
        }

        skeleton_free(module);
    }

    if (violations.count > 0) {
        log_error("Cannot extract because some extracted modules import project modules outside the selected PATH.");
        for (int i = 0; i < violations.count; i++) {
            ExtractViolation *v = &violations.violations[i];
            fprintf(stderr, "  - %s (%s) imports %s (%s)\n",
                    v->importing_module_name, v->importing_file_abs,
                    v->imported_module_name, v->imported_file_abs);
        }
        log_error("Hint: Extract a directory that includes these modules, or refactor your imports.");

        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    /* Phase G: Create TARGET_PATH and initialize package */
    if (!ensure_directory_exists_recursive(target_path)) {
        log_error("Failed to create directory: %s", target_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    /* Call shared init helper */
    int init_result = package_init_at_path(target_path, package_spec, true, true);
    if (init_result != 0) {
        log_error("Failed to initialize package at %s", target_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    /* Phase H: Move files to TARGET_PATH/src/ */
    char target_src[MAX_PATH_LENGTH];
    int target_src_len = snprintf(target_src, sizeof(target_src), "%s/src", target_path);
    if (target_src_len < 0 || target_src_len >= (int)sizeof(target_src)) {
        log_error("Target path too long");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    for (int i = 0; i < selected.count; i++) {
        const char *src_file = selected.abs_paths[i];
        const char *dest_relative = selected.dest_relatives[i];

        char dest_file[MAX_PATH_LENGTH];
        int dest_len = snprintf(dest_file, sizeof(dest_file), "%s/%s",
                               target_src, dest_relative);

        if (dest_len < 0 || dest_len >= (int)sizeof(dest_file)) {
            log_error("Destination path too long");
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }

        if (!move_file(src_file, dest_file)) {
            log_error("Failed to move %s -> %s", src_file, dest_file);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }
    }

    /* Phase I: Build exposed-modules list */
    char **exposed_modules = arena_malloc(elm_file_count * sizeof(char*));
    int exposed_count = 0;

    for (int i = 0; i < selected.count; i++) {
        if (!path_is_elm_file(selected.abs_paths[i])) {
            continue;
        }

        /* Use precomputed destination path */
        char dest_file[MAX_PATH_LENGTH];
        int dest_len = snprintf(dest_file, sizeof(dest_file), "%s/%s",
                               target_src, selected.dest_relatives[i]);
        if (dest_len < 0 || dest_len >= (int)sizeof(dest_file)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(dest_file);
        if (!module || !module->module_name) {
            if (module) skeleton_free(module);
            continue;
        }

        /* Check if module has an exposing clause */
        bool has_exposing = module->exports.expose_all ||
                           module->exports.values_count > 0 ||
                           module->exports.types_count > 0 ||
                           module->exports.types_with_constructors_count > 0;

        if (has_exposing) {
            /* Deduplicate */
            bool already_added = false;
            for (int j = 0; j < exposed_count; j++) {
                if (strcmp(exposed_modules[j], module->module_name) == 0) {
                    already_added = true;
                    break;
                }
            }

            if (!already_added) {
                exposed_modules[exposed_count++] = arena_strdup(module->module_name);
            }
        }

        skeleton_free(module);
    }

    /* Phase J: Update package elm.json with exposed-modules */
    char pkg_elm_json_path[MAX_PATH_LENGTH];
    snprintf(pkg_elm_json_path, sizeof(pkg_elm_json_path), "%s/elm.json", target_path);

    size_t elm_json_size = 0;
    char *elm_json_content = file_read_contents_bounded(pkg_elm_json_path,
                                                        MAX_ELM_JSON_FILE_BYTES,
                                                        &elm_json_size);
    if (!elm_json_content) {
        log_error("Failed to read %s", pkg_elm_json_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    cJSON *root = cJSON_Parse(elm_json_content);
    arena_free(elm_json_content);

    if (!root) {
        log_error("Failed to parse %s", pkg_elm_json_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    cJSON *exposed_array = cJSON_CreateArray();
    for (int i = 0; i < exposed_count; i++) {
        cJSON_AddItemToArray(exposed_array, cJSON_CreateString(exposed_modules[i]));
    }

    cJSON_ReplaceItemInObject(root, "exposed-modules", exposed_array);

    char *updated_json = cJSON_Print(root);
    cJSON_Delete(root);

    if (!updated_json) {
        log_error("Failed to serialize updated elm.json");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    FILE *out = fopen(pkg_elm_json_path, "w");
    if (!out) {
        log_error("Failed to open %s for writing", pkg_elm_json_path);
        arena_free(updated_json);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    fputs(updated_json, out);
    fclose(out);
    arena_free(updated_json);

    /* Phase K: Add package as local-dev dependency to application */
    char *target_abs = get_realpath_safe(target_path);
    if (!target_abs) {
        log_error("Failed to resolve absolute path for %s", target_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        arena_free(target_abs);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        arena_free(target_abs);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    int install_result = install_local_dev(target_abs, package_name, "elm.json",
                                          env, false, true);
    install_env_free(env);
    arena_free(target_abs);

    if (install_result != 0) {
        log_error("Package was created and files moved, but failed to add as dependency.");
        log_error("You can manually add it with: %s package install %s",
                 global_context_program_name(), package_name);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    printf("Successfully extracted %d file(s) to %s and added as local-dev dependency.\n",
           selected.count, target_path);

    if (version_str) arena_free(version_str);
    elm_json_free(app_json);

    return 0;
}
