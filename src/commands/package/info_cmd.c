#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../install_check.h"
#include "../../elm_json.h"
#include "../../elm_project.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../protocol_v1/install.h"
#include "../../protocol_v2/install.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../global_context.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../progname.h"
#include "../../fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void print_info_usage(void) {
    printf("Usage: %s package info [PATH | <author/package> [VERSION]]\n", program_name);
    printf("\n");
    printf("Display package management information.\n");
    printf("\n");
    printf("Shows:\n");
    printf("  - Current ELM_HOME directory\n");
    printf("  - Registry cache statistics\n");
    printf("  - Package registry connectivity\n");
    printf("  - Installed packages (if run in a project directory)\n");
    printf("  - Available updates (if run in a project directory)\n");
    printf("\n");
    printf("Version resolution (for package lookup):\n");
    printf("  - If package is in elm.json: uses that version\n");
    printf("  - If not in elm.json and no VERSION specified: uses latest from registry\n");
    printf("  - If VERSION specified: uses that specific version\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package info                  # Show general package info\n", program_name);
    printf("  %s package info ./path/to/dir    # Show info for elm.json at path\n", program_name);
    printf("  %s package info elm/core         # Show info for elm/core package\n", program_name);
    printf("  %s package info elm/http 2.0.0   # Show info for elm/http 2.0.0\n", program_name);
    printf("\n");
    printf("Note: Package name format (author/package) takes priority over paths.\n");
    printf("      Use './package/author' to treat as a path instead.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help                             # Show this help\n");
}

static bool is_package_name_format(const char *str) {
    if (!str || str[0] == '\0') {
        return false;
    }

    if (str[0] == '/' || (str[0] == '.' && str[1] == '/')) {
        return false;
    }

    int slash_count = 0;
    for (const char *p = str; *p; p++) {
        if (*p == '/') {
            slash_count++;
        }
    }

    return slash_count == 1;
}

/* Check if a version is a local-dev version (0.0.0 or 999.0.0) */
static bool is_local_dev_version(int major, int minor, int patch) {
    return (major == 0 && minor == 0 && patch == 0) ||
           (major == 999 && minor == 0 && patch == 0);
}

/**
 * Get list of application paths tracking a specific local-dev package.
 * Returns arena-allocated array of paths, or NULL if none found.
 */
static char **get_tracking_applications(const char *author, const char *name,
                                        const char *version, int *out_count) {
    *out_count = 0;

    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return NULL;
    }

    /* Build path: tracking_dir/author/name/version */
    size_t dir_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *version_dir = arena_malloc(dir_len);
    if (!version_dir) {
        arena_free(tracking_dir);
        return NULL;
    }
    snprintf(version_dir, dir_len, "%s/%s/%s/%s", tracking_dir, author, name, version);
    arena_free(tracking_dir);

    DIR *dir = opendir(version_dir);
    if (!dir) {
        arena_free(version_dir);
        return NULL;
    }

    /* Collect paths */
    int capacity = 16;
    int count = 0;
    char **paths = arena_malloc(capacity * sizeof(char *));
    if (!paths) {
        closedir(dir);
        arena_free(version_dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Read the tracking file to get the app elm.json path */
        size_t file_len = strlen(version_dir) + strlen(entry->d_name) + 2;
        char *tracking_file = arena_malloc(file_len);
        if (!tracking_file) continue;
        snprintf(tracking_file, file_len, "%s/%s", version_dir, entry->d_name);

        char *content = file_read_contents(tracking_file);
        arena_free(tracking_file);
        if (!content) continue;

        /* Strip trailing newline */
        size_t content_len = strlen(content);
        if (content_len > 0 && content[content_len - 1] == '\n') {
            content[content_len - 1] = '\0';
        }

        /* Check if the elm.json still exists */
        struct stat st;
        if (stat(content, &st) != 0) {
            arena_free(content);
            continue;
        }

        /* Add to list */
        if (count >= capacity) {
            capacity *= 2;
            paths = arena_realloc(paths, capacity * sizeof(char *));
            if (!paths) {
                arena_free(content);
                break;
            }
        }
        paths[count++] = content;
    }

    closedir(dir);
    arena_free(version_dir);

    *out_count = count;
    return count > 0 ? paths : NULL;
}

/**
 * Print tracking information for a package.
 * Shows which applications are tracking this package for local development.
 */
static void print_package_tracking_info(const char *author, const char *name, const char *version) {
    int app_count = 0;
    char **app_paths = get_tracking_applications(author, name, version, &app_count);

    if (app_count > 0 && app_paths) {
        printf("\nLocal development tracked by:\n\n");
        for (int i = 0; i < app_count; i++) {
            /* Get directory path (remove /elm.json if present) */
            char *path_copy = arena_strdup(app_paths[i]);
            char *elm_json_pos = strstr(path_copy, "/elm.json");
            if (elm_json_pos) {
                *elm_json_pos = '\0';
            }
            printf("  %s\n", path_copy);
            arena_free(path_copy);
            arena_free(app_paths[i]);
        }
        arena_free(app_paths);
    }
}

/**
 * Helper to check if a package has a local-dev version and add it to the list.
 */
static bool add_local_dev_package_to_list(Package *pkg, char ***packages, int *count, int *capacity) {
    if (!pkg || !pkg->version) return true;

    int major, minor, patch;
    if (sscanf(pkg->version, "%d.%d.%d", &major, &minor, &patch) == 3) {
        if (is_local_dev_version(major, minor, patch)) {
            if (*count >= *capacity) {
                *capacity *= 2;
                *packages = arena_realloc(*packages, (*capacity) * sizeof(char *));
                if (!*packages) return false;
            }

            /* Format: "author/name version" */
            size_t len = strlen(pkg->author) + strlen(pkg->name) + strlen(pkg->version) + 10;
            char *pkg_str = arena_malloc(len);
            if (pkg_str) {
                snprintf(pkg_str, len, "%s/%s %s", pkg->author, pkg->name, pkg->version);
                (*packages)[(*count)++] = pkg_str;
            }
        }
    }
    return true;
}

/**
 * Get list of packages being tracked for local development by an application.
 * Returns arena-allocated array of "author/name version" strings.
 */
static char **get_tracked_packages(const char *elm_json_path, int *out_count) {
    *out_count = 0;

    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json || elm_json->type != ELM_PROJECT_APPLICATION) {
        if (elm_json) elm_json_free(elm_json);
        return NULL;
    }

    /* Collect packages with local-dev versions */
    int capacity = 16;
    int count = 0;
    char **packages = arena_malloc(capacity * sizeof(char *));
    if (!packages) {
        elm_json_free(elm_json);
        return NULL;
    }

    /* Check all dependency maps */
    if (elm_json->dependencies_direct) {
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            if (!add_local_dev_package_to_list(&elm_json->dependencies_direct->packages[i],
                                               &packages, &count, &capacity)) {
                elm_json_free(elm_json);
                return NULL;
            }
        }
    }
    if (elm_json->dependencies_indirect) {
        for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
            if (!add_local_dev_package_to_list(&elm_json->dependencies_indirect->packages[i],
                                               &packages, &count, &capacity)) {
                elm_json_free(elm_json);
                return NULL;
            }
        }
    }
    if (elm_json->dependencies_test_direct) {
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            if (!add_local_dev_package_to_list(&elm_json->dependencies_test_direct->packages[i],
                                               &packages, &count, &capacity)) {
                elm_json_free(elm_json);
                return NULL;
            }
        }
    }
    if (elm_json->dependencies_test_indirect) {
        for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
            if (!add_local_dev_package_to_list(&elm_json->dependencies_test_indirect->packages[i],
                                               &packages, &count, &capacity)) {
                elm_json_free(elm_json);
                return NULL;
            }
        }
    }

    elm_json_free(elm_json);

    *out_count = count;
    return count > 0 ? packages : NULL;
}

/**
 * Print list of packages being tracked for local development by an application.
 */
static void print_application_tracking_info(const char *elm_json_path) {
    int pkg_count = 0;
    char **packages = get_tracked_packages(elm_json_path, &pkg_count);

    if (pkg_count > 0 && packages) {
        printf("\nTracking local dev packages:\n");
        for (int i = 0; i < pkg_count; i++) {
            printf("  %s (local)\n", packages[i]);
            arena_free(packages[i]);
        }
        arena_free(packages);
    }
}

static int show_package_info_from_registry(const char *package_name, const char *version_arg, InstallEnv *env) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    if (global_context_is_v2() && env->v2_registry) {
        V2PackageEntry *entry = v2_registry_find(env->v2_registry, author, name);
        if (!entry) {
            fprintf(stderr, "Error: Package '%s/%s' not found in registry\n", author, name);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        if (entry->version_count == 0) {
            fprintf(stderr, "Error: Package '%s/%s' has no versions\n", author, name);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        const char *version_to_use = NULL;
        bool version_found = false;
        char *allocated_version = NULL;

        if (version_arg) {
            int major, minor, patch;
            if (sscanf(version_arg, "%d.%d.%d", &major, &minor, &patch) == 3) {
                for (size_t i = 0; i < entry->version_count; i++) {
                    V2PackageVersion *v = &entry->versions[i];
                    if (v->major == (uint16_t)major && 
                        v->minor == (uint16_t)minor && 
                        v->patch == (uint16_t)patch) {
                        version_to_use = version_arg;
                        version_found = true;
                        break;
                    }
                }
            }

            if (!version_found) {
                fprintf(stderr, "Error: Version %s not found for package %s/%s\n", version_arg, author, name);
                printf("\nAvailable versions:\n");
                for (size_t i = 0; i < entry->version_count; i++) {
                    V2PackageVersion *v = &entry->versions[i];
                    if (v->status == V2_STATUS_VALID) {
                        printf("  %u.%u.%u\n", v->major, v->minor, v->patch);
                    }
                }
                printf("\n");
                arena_free(author);
                arena_free(name);
                return 1;
            }
        } else {
            ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
            if (elm_json) {
                Package *existing_pkg = find_existing_package(elm_json, author, name);
                if (existing_pkg && existing_pkg->version) {
                    if (!strchr(existing_pkg->version, ' ')) {
                        allocated_version = arena_strdup(existing_pkg->version);
                        version_to_use = allocated_version;
                        version_found = true;
                    }
                }
                elm_json_free(elm_json);
            }

            if (!version_found && entry->version_count > 0) {
                for (size_t i = 0; i < entry->version_count; i++) {
                    V2PackageVersion *v = &entry->versions[i];
                    if (v->status == V2_STATUS_VALID) {
                        allocated_version = arena_malloc(32);
                        snprintf(allocated_version, 32, "%u.%u.%u", v->major, v->minor, v->patch);
                        version_to_use = allocated_version;
                        version_found = true;
                        break;
                    }
                }
            }
        }

        if (!version_found || !version_to_use) {
            fprintf(stderr, "Error: Could not determine version for %s/%s\n", author, name);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        /* Check if this is a local-dev package */
        int v_major, v_minor, v_patch;
        bool is_local_dev = false;
        if (sscanf(version_to_use, "%d.%d.%d", &v_major, &v_minor, &v_patch) == 3) {
            is_local_dev = is_local_dev_version(v_major, v_minor, v_patch);
        }

        char latest_buf[32];
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *v = &entry->versions[i];
            if (v->status == V2_STATUS_VALID) {
                snprintf(latest_buf, sizeof(latest_buf), "%u.%u.%u", v->major, v->minor, v->patch);
                break;
            }
        }

        printf("\nPackage: %s/%s\n", author, name);
        if (is_local_dev) {
            printf("Version: %s (local development)\n", version_to_use);
        } else {
            printf("Version: %s\n", version_to_use);
        }
        if (strcmp(version_to_use, latest_buf) != 0) {
            printf("Latest version: %s\n", latest_buf);
        }
        printf("Total versions: %zu\n", entry->version_count);
        printf("\n");

        int result = v2_show_package_dependencies(author, name, version_to_use, env->v2_registry);

        /* Show local development tracking information */
        if (is_local_dev) {
            print_package_tracking_info(author, name, version_to_use);
        }

        if (allocated_version) {
            arena_free(allocated_version);
        }
        arena_free(author);
        arena_free(name);;
        return result;
    }

    RegistryEntry *registry_entry = registry_find(env->registry, author, name);
    if (!registry_entry) {
        fprintf(stderr, "Error: Package '%s/%s' not found in registry\n", author, name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    if (registry_entry->version_count == 0) {
        fprintf(stderr, "Error: Package '%s/%s' has no versions\n", author, name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    const char *version_to_use = NULL;
    bool version_found = false;
    char *allocated_version = NULL;

    if (version_arg) {
        for (size_t i = 0; i < registry_entry->version_count; i++) {
            char *v_str = version_to_string(&registry_entry->versions[i]);
            if (v_str && strcmp(v_str, version_arg) == 0) {
                version_to_use = version_arg;
                version_found = true;
                arena_free(v_str);
                break;
            }
            if (v_str) {
                arena_free(v_str);
            }
        }

        if (!version_found) {
            fprintf(stderr, "Error: Version %s not found for package %s/%s\n", version_arg, author, name);
            printf("\nAvailable versions:\n");
            for (size_t i = 0; i < registry_entry->version_count; i++) {
                char *v_str = version_to_string(&registry_entry->versions[i]);
                if (v_str) {
                    printf("  %s\n", v_str);
                    arena_free(v_str);
                }
            }
            printf("\n");
            arena_free(author);
            arena_free(name);
            return 1;
        }
    } else {
        ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
        if (elm_json) {
            Package *existing_pkg = find_existing_package(elm_json, author, name);
            if (existing_pkg && existing_pkg->version) {
                if (!strchr(existing_pkg->version, ' ')) {
                    allocated_version = arena_strdup(existing_pkg->version);
                    version_to_use = allocated_version;
                    version_found = true;
                }
            }
            elm_json_free(elm_json);
        }

        if (!version_found && registry_entry->version_count > 0) {
            allocated_version = version_to_string(&registry_entry->versions[0]);
            version_to_use = allocated_version;
            version_found = true;
        }
    }

    if (!version_found || !version_to_use) {
        fprintf(stderr, "Error: Could not determine version for %s/%s\n", author, name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    /* Check if this is a local-dev package */
    int v_major, v_minor, v_patch;
    bool is_local_dev = false;
    if (sscanf(version_to_use, "%d.%d.%d", &v_major, &v_minor, &v_patch) == 3) {
        is_local_dev = is_local_dev_version(v_major, v_minor, v_patch);
    }

    char *latest_version = version_to_string(&registry_entry->versions[0]);

    printf("\nPackage: %s/%s\n", author, name);
    if (is_local_dev) {
        printf("Version: %s (local development)\n", version_to_use);
    } else {
        printf("Version: %s\n", version_to_use);
    }
    if (latest_version && strcmp(version_to_use, latest_version) != 0) {
        printf("Latest version: %s\n", latest_version);
    }
    printf("Total versions: %zu\n", registry_entry->version_count);
    printf("\n");

    int result = v1_show_package_dependencies(author, name, version_to_use, env);

    /* Show local development tracking information */
    if (is_local_dev) {
        print_package_tracking_info(author, name, version_to_use);
    }

    if (latest_version) {
        arena_free(latest_version);
    }
    if (allocated_version) {
        arena_free(allocated_version);
    }
    arena_free(author);
    arena_free(name);
    return result;
}

int cmd_info(int argc, char *argv[]) {
    const char *arg = NULL;
    const char *version_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_info_usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_info_usage();
            return 1;
        } else {
            if (!arg) {
                arg = argv[i];
            } else if (!version_arg) {
                version_arg = argv[i];
            } else {
                fprintf(stderr, "Error: Too many arguments\n");
                print_info_usage();
                return 1;
            }
        }
    }

    const char *elm_json_path = NULL;
    bool is_package_lookup = false;

    if (arg) {
        if (is_package_name_format(arg)) {
            is_package_lookup = true;
        } else {
            if (version_arg) {
                fprintf(stderr, "Error: Version argument is only valid with package name (author/package)\n");
                print_info_usage();
                return 1;
            }
            struct stat st;
            char path_buf[PATH_MAX];

            if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(path_buf, sizeof(path_buf), "%s/elm.json", arg);
            } else if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(path_buf, sizeof(path_buf), "%s", arg);
            } else {
                fprintf(stderr, "Error: Path does not exist: %s\n", arg);
                return 1;
            }

            if (stat(path_buf, &st) != 0 || !S_ISREG(st.st_mode)) {
                fprintf(stderr, "Error: elm.json not found at: %s\n", path_buf);
                return 1;
            }

            elm_json_path = arena_strdup(path_buf);
        }
    } else {
        elm_json_path = ELM_JSON_PATH;
    }

    if (is_package_lookup) {
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

        int result = show_package_info_from_registry(arg, version_arg, env);
        install_env_free(env);
        return result;
    }

    V2Registry *v2_registry = NULL;
    if (global_context_is_v2()) {
        GlobalContext *ctx = global_context_get();
        size_t index_path_len = strlen(ctx->repository_path) + strlen("/index.dat") + 1;
        char *index_path = arena_malloc(index_path_len);
        if (index_path) {
            snprintf(index_path, index_path_len, "%s/index.dat", ctx->repository_path);
            v2_registry = v2_registry_load_from_zip(index_path);
            arena_free(index_path);
        }
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        if (v2_registry) {
            v2_registry_free(v2_registry);
        }
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        if (v2_registry) {
            v2_registry_free(v2_registry);
        }
        return 1;
    }

    ElmJson *elm_json = elm_json_read(elm_json_path);

    if (elm_json) {

        printf("\n");
        int total_packages = 0;
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            total_packages = elm_json->dependencies_direct->count +
                           elm_json->dependencies_indirect->count +
                           elm_json->dependencies_test_direct->count +
                           elm_json->dependencies_test_indirect->count;
            printf("Application\n");
            printf("-------------------\n");
            printf("Installed packages:\n");
            printf("  Direct dependencies:   %4d\n", elm_json->dependencies_direct->count);
            printf("  Indirect dependencies: %4d\n", elm_json->dependencies_indirect->count);
            printf("  Test direct:           %4d\n", elm_json->dependencies_test_direct->count);
            printf("  Test indirect:         %4d\n", elm_json->dependencies_test_indirect->count);
        } else {
            total_packages = elm_json->package_dependencies->count +
                           elm_json->package_test_dependencies->count;
            printf("Package\n");
            printf("-------------------\n");

            /* Show package metadata */
            if (elm_json->package_name) {
                printf("Name: %s\n", elm_json->package_name);
            }
            if (elm_json->package_version) {
                printf("Version: %s\n", elm_json->package_version);
            }

            /* Show exposed modules */
            int exposed_count = 0;
            char **exposed_modules = elm_parse_exposed_modules(elm_json_path, &exposed_count);
            if (exposed_count > 0 && exposed_modules) {
                printf("\nExposed modules:\n");
                for (int i = 0; i < exposed_count; i++) {
                    printf("  %s\n", exposed_modules[i]);
                    arena_free(exposed_modules[i]);
                }
                arena_free(exposed_modules);
            }

            printf("\n");
            printf("Dependencies:           %4d\n", elm_json->package_dependencies->count);
            printf("Test dependencies:      %4d\n", elm_json->package_test_dependencies->count);
        }
        printf("Total:                  %4d\n", total_packages);

        size_t max_name_len = 0;
        if (global_context_is_v2() && v2_registry) {
            max_name_len = get_max_upgrade_name_len_v2(elm_json_path, v2_registry);
        } else {
            max_name_len = get_max_upgrade_name_len(elm_json_path, env->registry);
        }

        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
                Package *pkg = &elm_json->dependencies_direct->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                size_t len = strlen(full_name);
                if (len > max_name_len) max_name_len = len;
            }
            for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_indirect->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                size_t len = strlen(full_name);
                if (len > max_name_len) max_name_len = len;
            }
            for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
                Package *pkg = &elm_json->dependencies_test_direct->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                size_t len = strlen(full_name);
                if (len > max_name_len) max_name_len = len;
            }
            for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                size_t len = strlen(full_name);
                if (len > max_name_len) max_name_len = len;
            }
        } else {
            for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                Package *pkg = &elm_json->package_dependencies->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                size_t len = strlen(full_name);
                if (len > max_name_len) max_name_len = len;
            }
            for (int i = 0; i < elm_json->package_test_dependencies->count; i++) {
                Package *pkg = &elm_json->package_test_dependencies->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                size_t len = strlen(full_name);
                if (len > max_name_len) max_name_len = len;
            }
        }

        printf("\nInstalled Package Versions:\n\n");
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
                Package *pkg = &elm_json->dependencies_direct->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s  %s\n", (int)max_name_len, full_name, pkg->version);
            }

            if (elm_json->dependencies_direct->count > 0 && elm_json->dependencies_indirect->count > 0) {
                printf("\n");
            }

            for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_indirect->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s  %s (indirect)\n", (int)max_name_len, full_name, pkg->version);
            }

            if ((elm_json->dependencies_direct->count > 0 || elm_json->dependencies_indirect->count > 0) &&
                (elm_json->dependencies_test_direct->count > 0 || elm_json->dependencies_test_indirect->count > 0)) {
                printf("\n");
            }

            for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
                Package *pkg = &elm_json->dependencies_test_direct->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s  %s (test)\n", (int)max_name_len, full_name, pkg->version);
            }

            if (elm_json->dependencies_test_direct->count > 0 && elm_json->dependencies_test_indirect->count > 0) {
                printf("\n");
            }

            for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s  %s (test, indirect)\n", (int)max_name_len, full_name, pkg->version);
            }
        } else {
            for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                Package *pkg = &elm_json->package_dependencies->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s  %s\n", (int)max_name_len, full_name, pkg->version);
            }

            if (elm_json->package_dependencies->count > 0 && elm_json->package_test_dependencies->count > 0) {
                printf("\n");
            }

            for (int i = 0; i < elm_json->package_test_dependencies->count; i++) {
                Package *pkg = &elm_json->package_test_dependencies->packages[i];
                char full_name[256];
                snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s  %s (test)\n", (int)max_name_len, full_name, pkg->version);
            }
        }

        printf("\n");

        if (global_context_is_v2() && v2_registry) {
            check_all_upgrades_v2(elm_json_path, v2_registry, max_name_len);
        } else {
            check_all_upgrades(elm_json_path, env->registry, max_name_len);
        }

        printf("\n");


        /* Show local development tracking information */
        if (elm_json) {
            if (elm_json->type == ELM_PROJECT_APPLICATION) {
                print_application_tracking_info(elm_json_path);
            } else if (elm_json->type == ELM_PROJECT_PACKAGE && elm_json->package_name) {
                /* For packages, show which applications are tracking this package */
                char *author = NULL;
                char *name = NULL;
                if (parse_package_name(elm_json->package_name, &author, &name)) {
                    /* Check if this package has a local-dev version */
                    if (elm_json->package_version) {
                        int major, minor, patch;
                        if (sscanf(elm_json->package_version, "%d.%d.%d", &major, &minor, &patch) == 3) {
                            if (is_local_dev_version(major, minor, patch)) {
                                print_package_tracking_info(author, name, elm_json->package_version);
                            }
                        }
                    }
                    arena_free(author);
                    arena_free(name);
                }
            }
        }

        printf("\n");
    } else {
        printf("\n");
        printf("Package Management Information\n");
        printf("===============================\n");
    }

    printf("\nELM_HOME: %s\n", env->cache->elm_home);

    if (global_context_is_v2() && v2_registry) {
        printf("\nV2 Registry:\n");
        printf("  Location: %s/index.dat\n", global_context_get()->repository_path);
        printf("  Packages: %zu\n", v2_registry->entry_count);
        size_t total_versions = 0;
        for (size_t i = 0; i < v2_registry->entry_count; i++) {
            total_versions += v2_registry->entries[i].version_count;
        }
        printf("  Versions: %zu\n", total_versions);
        printf("  Status: Local (V2 protocol)\n");
    } else {
        printf("\nRegistry Cache:\n");
        printf("  Location: %s\n", env->cache->registry_path);
        printf("  Packages: %zu\n", env->registry->entry_count);
        printf("  Versions: %zu\n", env->registry->total_versions);

        printf("\nRegistry URL: %s\n", env->registry_url);
        if (env->offline) {
            printf("  Status: Offline (using cached data)\n");
        } else {
            printf("  Status: Connected\n");
        }
    }

    printf("\n");

    if (elm_json) {
        elm_json_free(elm_json);
    }

    install_env_free(env);

    if (v2_registry) {
        v2_registry_free(v2_registry);
    }

    return 0;
}
