#include "package_common.h"
#include "../../install.h"
#include "../../install_check.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../protocol_v1/install.h"
#include "../../protocol_v2/install.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../global_context.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../progname.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <limits.h>

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

static int show_package_info_from_registry(const char *package_name, const char *version_arg, InstallEnv *env, V2Registry *v2_registry) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    if (global_context_is_v2() && v2_registry) {
        V2PackageEntry *entry = v2_registry_find(v2_registry, author, name);
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

        char latest_buf[32];
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *v = &entry->versions[i];
            if (v->status == V2_STATUS_VALID) {
                snprintf(latest_buf, sizeof(latest_buf), "%u.%u.%u", v->major, v->minor, v->patch);
                break;
            }
        }

        printf("\nPackage: %s/%s\n", author, name);
        printf("Version: %s\n", version_to_use);
        if (strcmp(version_to_use, latest_buf) != 0) {
            printf("Latest version: %s\n", latest_buf);
        }
        printf("Total versions: %zu\n", entry->version_count);
        printf("\n");

        int result = v2_show_package_dependencies(author, name, version_to_use, v2_registry);

        if (allocated_version) {
            arena_free(allocated_version);
        }
        arena_free(author);
        arena_free(name);
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

    char *latest_version = version_to_string(&registry_entry->versions[0]);

    printf("\nPackage: %s/%s\n", author, name);
    printf("Version: %s\n", version_to_use);
    if (latest_version && strcmp(version_to_use, latest_version) != 0) {
        printf("Latest version: %s\n", latest_version);
    }
    printf("Total versions: %zu\n", registry_entry->version_count);
    printf("\n");

    int result = v1_show_package_dependencies(author, name, version_to_use, env);

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

    if (is_package_lookup) {
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

        int result = show_package_info_from_registry(arg, version_arg, env, v2_registry);
        install_env_free(env);
        if (v2_registry) {
            v2_registry_free(v2_registry);
        }
        return result;
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
        printf("\nProject Information\n");
        printf("-------------------\n");

        int total_packages = 0;
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            total_packages = elm_json->dependencies_direct->count +
                           elm_json->dependencies_indirect->count +
                           elm_json->dependencies_test_direct->count +
                           elm_json->dependencies_test_indirect->count;
            printf("Project type: Application\n");
            printf("Installed packages:\n");
            printf("  Direct dependencies:     %d\n", elm_json->dependencies_direct->count);
            printf("  Indirect dependencies:   %d\n", elm_json->dependencies_indirect->count);
            printf("  Test direct:             %d\n", elm_json->dependencies_test_direct->count);
            printf("  Test indirect:           %d\n", elm_json->dependencies_test_indirect->count);
        } else {
            total_packages = elm_json->package_dependencies->count +
                           elm_json->package_test_dependencies->count;
            printf("Project type: Package\n");
            printf("Installed packages:\n");
            printf("  Dependencies:      %d\n", elm_json->package_dependencies->count);
            printf("  Test dependencies: %d\n", elm_json->package_test_dependencies->count);
        }
        printf("  Total:                   %d\n", total_packages);

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

        printf("\nInstalled Package Versions:\n");
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
    } else {
        printf("\n");
        printf("Package Management Information\n");
        printf("===============================\n");
    }

    printf("\nELM_HOME: %s\n", env->cache->elm_home);

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
