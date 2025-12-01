#include "install.h"
#include "install_check.h"
#include "elm_json.h"
#include "cache.h"
#include "solver.h"
#include "install_env.h"
#include "registry.h"
#include "http_client.h"
#include "alloc.h"
#include "log.h"
#include "progname.h"
#include "fileutil.h"
#include "commands/cache/check/cache_check.h"
#include "commands/cache/full_scan/cache_full_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <unistd.h>

#define ELM_JSON_PATH "elm.json"

#define ANSI_DULL_CYAN "\033[36m"
#define ANSI_DULL_YELLOW "\033[33m"
#define ANSI_RESET "\033[0m"

static char* find_package_elm_json(const char *pkg_path);

static void print_install_what(const char *elm_home) {
    fprintf(stderr, "%s-- INSTALL WHAT? ---------------------------------------------------------------%s\n\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "I am expecting commands like:\n\n");
    fprintf(stderr, "    elm install elm/http\n");
    fprintf(stderr, "    elm install elm/json\n");
    fprintf(stderr, "    elm install elm/random\n\n");
    fprintf(stderr, "Hint: In JavaScript folks run `npm install` to start projects. \"Gotta download\n");
    fprintf(stderr, "everything!\" But why download packages again and again? Instead, Elm caches\n");
    fprintf(stderr, "packages in %s%s%s so each one is downloaded and built ONCE on\n",
            ANSI_DULL_YELLOW, elm_home ? elm_home : "$ELM_HOME", ANSI_RESET);
    fprintf(stderr, "your machine. Elm projects check that cache before trying the internet. This\n");
    fprintf(stderr, "reduces build times, reduces server costs, and makes it easier to work offline.\n");
    fprintf(stderr, "As a result %selm install%s is only for adding dependencies to elm.json, whereas\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "%selm make%s is in charge of gathering dependencies and building everything. So\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "maybe try elm make instead?\n\n");
}

static void print_install_usage(void) {
    printf("Usage: %s install [PACKAGE]\n", program_name);
    printf("\n");
    printf("Install packages for your Elm project.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s install elm/html              # Add elm/html to your project\n", program_name);
    printf("  %s install elm/json --test       # Add elm/json as a test dependency\n", program_name);
    printf("  %s install --major elm/html      # Upgrade elm/html to next major version\n", program_name);
    printf("  %s install --from-file ./pkg.zip elm/html  # Install from local file\n", program_name);
    printf("  %s install --from-url <url> elm/html       # Install from URL\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --test                             # Install as test dependency\n");
    printf("  --major <package>                  # Allow major version upgrade for package\n");
    printf("  --from-file <path> <package>       # Install from local file/directory\n");
    printf("  --from-url <url> <package>         # Install from URL (skip SHA check)\n");
    printf("  --pin                              # Create PIN file with package version\n");
    printf("  -v, --verbose                      # Show progress reports (registry, connectivity)\n");
    printf("  -q, --quiet                        # Suppress progress reports\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
}

static bool parse_package_name(const char *package, char **author, char **name) {
    if (!package) return false;

    const char *slash = strchr(package, '/');
    if (!slash) {
        fprintf(stderr, "Error: Package name must be in format 'author/package'\n");
        return false;
    }

    size_t author_len = slash - package;
    *author = arena_malloc(author_len + 1);
    if (!*author) return false;
    strncpy(*author, package, author_len);
    (*author)[author_len] = '\0';

    *name = arena_strdup(slash + 1);
    if (!*name) {
        arena_free(*author);
        return false;
    }

    return true;
}

static void parse_version(const char *version, int *major, int *minor, int *patch) {
    *major = 0;
    *minor = 0;
    *patch = 0;

    if (version) {
        sscanf(version, "%d.%d.%d", major, minor, patch);
    }
}

static Package* find_existing_package(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || !author || !name) {
        return NULL;
    }

    Package *pkg = NULL;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        pkg = package_map_find(elm_json->dependencies_direct, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_indirect, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_test_direct, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_test_indirect, author, name);
        if (pkg) return pkg;
    } else {
        pkg = package_map_find(elm_json->package_dependencies, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->package_test_dependencies, author, name);
        if (pkg) return pkg;
    }

    return NULL;
}

static bool read_package_info_from_elm_json(const char *elm_json_path, char **out_author, char **out_name, char **out_version) {
    ElmJson *pkg_elm_json = elm_json_read(elm_json_path);
    if (!pkg_elm_json) {
        return false;
    }

    if (pkg_elm_json->type != ELM_PROJECT_PACKAGE) {
        fprintf(stderr, "Error: The elm.json at %s is not a package project\n", elm_json_path);
        elm_json_free(pkg_elm_json);
        return false;
    }

    if (pkg_elm_json->package_name) {
        if (!parse_package_name(pkg_elm_json->package_name, out_author, out_name)) {
            elm_json_free(pkg_elm_json);
            return false;
        }
    } else {
        fprintf(stderr, "Error: No package name found in elm.json\n");
        elm_json_free(pkg_elm_json);
        return false;
    }

    if (pkg_elm_json->package_version) {
        *out_version = arena_strdup(pkg_elm_json->package_version);
    } else {
        fprintf(stderr, "Error: No version found in elm.json\n");
        elm_json_free(pkg_elm_json);
        return false;
    }

    elm_json_free(pkg_elm_json);
    return true;
}

static bool create_pin_file(const char *pkg_path, const char *version) {
    size_t pin_path_len = strlen(pkg_path) + strlen("/PIN") + 1;
    char *pin_path = arena_malloc(pin_path_len);
    if (!pin_path) {
        return false;
    }
    snprintf(pin_path, pin_path_len, "%s/PIN", pkg_path);

    FILE *pin_file = fopen(pin_path, "w");
    if (!pin_file) {
        fprintf(stderr, "Warning: Failed to create PIN file at %s\n", pin_path);
        arena_free(pin_path);
        return false;
    }

    fprintf(pin_file, "%s\n", version);
    fclose(pin_file);
    arena_free(pin_path);
    return true;
}

static bool ensure_path_exists(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    char *mutable_path = arena_strdup(path);
    if (!mutable_path) {
        return false;
    }

    bool ok = true;
    size_t len = strlen(mutable_path);
    for (size_t i = 1; i < len && ok; i++) {
        if (mutable_path[i] == '/' || mutable_path[i] == '\\') {
            char saved = mutable_path[i];
            mutable_path[i] = '\0';
            struct stat st;
            if (mutable_path[0] != '\0' && stat(mutable_path, &st) != 0) {
                if (mkdir(mutable_path, 0755) != 0) {
                    ok = false;
                }
            }
            mutable_path[i] = saved;
        }
    }

    if (ok) {
        struct stat st;
        if (stat(mutable_path, &st) != 0) {
            if (mkdir(mutable_path, 0755) != 0) {
                ok = false;
            }
        }
    }

    arena_free(mutable_path);
    return ok;
}

static bool install_from_file(const char *source_path, InstallEnv *env, const char *author, const char *name, const char *version) {
    struct stat st;
    if (stat(source_path, &st) != 0) {
        fprintf(stderr, "Error: Path does not exist: %s\n", source_path);
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Source path must be a directory\n");
        return false;
    }

    size_t pkg_base_len = strlen(env->cache->packages_dir) + strlen(author) + strlen(name) + 3;
    char *pkg_base_dir = arena_malloc(pkg_base_len);
    if (!pkg_base_dir) {
        fprintf(stderr, "Error: Failed to allocate package base directory\n");
        return false;
    }
    snprintf(pkg_base_dir, pkg_base_len, "%s/%s/%s", env->cache->packages_dir, author, name);

    char *dest_path = cache_get_package_path(env->cache, author, name, version);
    if (!dest_path) {
        fprintf(stderr, "Error: Failed to get package path\n");
        arena_free(pkg_base_dir);
        return false;
    }

    if (!ensure_path_exists(pkg_base_dir)) {
        fprintf(stderr, "Error: Failed to create package base directory: %s\n", pkg_base_dir);
        arena_free(pkg_base_dir);
        arena_free(dest_path);
        return false;
    }

    if (stat(dest_path, &st) == 0) {
        if (!remove_directory_recursive(dest_path)) {
            fprintf(stderr, "Warning: Failed to remove existing directory: %s\n", dest_path);
        }
    }

    // Copy from source to destination
    // Note: source_path could be either:
    //   1. A direct package directory (elm.json at root)
    //   2. An extracted zip with package in subdirectory
    // We handle both cases by checking for elm.json location
    char elm_json_check[PATH_MAX];
    snprintf(elm_json_check, sizeof(elm_json_check), "%s/elm.json", source_path);

    bool result;
    if (stat(elm_json_check, &st) == 0) {
        // elm.json at root - direct package directory
        result = copy_directory_selective(source_path, dest_path);
    } else {
        // elm.json not at root - likely extracted zip with subdirectory
        char *extracted_dir = find_first_subdirectory(source_path);
        if (!extracted_dir) {
            fprintf(stderr, "Error: Could not find package directory in %s\n", source_path);
            arena_free(pkg_base_dir);
            arena_free(dest_path);
            return false;
        }

        result = copy_directory_selective(extracted_dir, dest_path);
        arena_free(extracted_dir);
    }

    if (!result) {
        fprintf(stderr, "Error: Failed to install package to destination\n");
        arena_free(pkg_base_dir);
        arena_free(dest_path);
        return false;
    }

    char src_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "%s/src", dest_path);
    if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Package installation failed - no src directory found at %s\n", src_path);
        arena_free(pkg_base_dir);
        arena_free(dest_path);
        return false;
    }

    arena_free(pkg_base_dir);
    arena_free(dest_path);
    return true;
}

static int compare_package_changes(const void *a, const void *b) {
    const PackageChange *pkg_a = (const PackageChange *)a;
    const PackageChange *pkg_b = (const PackageChange *)b;

    int author_cmp = strcmp(pkg_a->author, pkg_b->author);
    if (author_cmp != 0) return author_cmp;

    return strcmp(pkg_a->name, pkg_b->name);
}

static int install_package(const char *package, bool is_test, bool major_upgrade, bool auto_yes, ElmJson *elm_json, InstallEnv *env) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package, &author, &name)) {
        return 1;
    }

    log_debug("Installing %s/%s%s%s", author, name,
              is_test ? " (test dependency)" : "",
              major_upgrade ? " (major upgrade allowed)" : "");

    Package *existing_pkg = find_existing_package(elm_json, author, name);
    PromotionType promotion = elm_json_find_package(elm_json, author, name);

    if (existing_pkg && !major_upgrade) {
        log_debug("Package %s/%s is already in your dependencies", author, name);
        if (existing_pkg->version &&
            cache_package_exists(env->cache, author, name, existing_pkg->version)) {
            log_debug("Package already downloaded");
        } else {
            log_debug("Package not downloaded yet");
        }

        if (promotion != PROMOTION_NONE) {
            if (elm_json_promote_package(elm_json, author, name)) {
                log_debug("Saving updated elm.json");
                if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
                    log_error("Failed to write elm.json");
                    arena_free(author);
                    arena_free(name);
                    return 1;
                }
                log_debug("Done");
            }
        } else {
            printf("It is already installed!\n");
        }

        arena_free(author);
        arena_free(name);
        return 0;
    } else if (existing_pkg && major_upgrade) {
        log_debug("Package %s/%s exists at %s, checking for major upgrade", 
                  author, name, existing_pkg->version);
    }

    RegistryEntry *registry_entry = registry_find(env->registry, author, name);

    if (!registry_entry) {
        //TODO: Suggest similar package names, check for misspellings
        log_error("I cannot find package '%s/%s'", author, name);
        log_error("Make sure the package name is correct");

        arena_free(author);
        arena_free(name);
        return 1;
    }

    log_debug("Found package in registry with %zu version(s)", registry_entry->version_count);

    /* Note: We do NOT download packages here. The solver will download elm.json
     * files on-demand as it explores the dependency graph. Full package downloads
     * happen only after solving is complete.
     */

    // Package is new, need to solve for compatible version
    log_debug("Resolving dependencies");

    SolverState *solver = solver_init(env, true);
    if (!solver) {
        log_error("Failed to initialize solver");
        arena_free(author);
        arena_free(name);
        return 1;
    }

    InstallPlan *out_plan = NULL;
    SolverResult result = solver_add_package(solver, elm_json, author, name, is_test, major_upgrade, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to resolve dependencies");

        switch (result) {
            case SOLVER_NO_SOLUTION:
                log_error("No compatible version found for %s/%s", author, name);
                break;
            case SOLVER_NO_OFFLINE_SOLUTION:
                log_error("Cannot solve offline (no cached registry)");
                break;
            case SOLVER_NETWORK_ERROR:
                log_error("Network error while downloading packages");
                break;
            case SOLVER_INVALID_PACKAGE:
                log_error("Invalid package specification");
                break;
            default:
                break;
        }

        arena_free(author);
        arena_free(name);
        return 1;
    }

    int add_count = 0;
    int change_count = 0;
    int max_width = 0;  // Use same width for both sections

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        int pkg_len = strlen(change->author) + 1 + strlen(change->name); // author/name

        if (!change->old_version) {
            add_count++;
        } else {
            change_count++;
        }

        if (pkg_len > max_width) max_width = pkg_len;
    }

    PackageChange *adds = arena_malloc(sizeof(PackageChange) * add_count);
    PackageChange *changes = arena_malloc(sizeof(PackageChange) * change_count);

    int add_idx = 0;
    int change_idx = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        if (!change->old_version) {
            adds[add_idx++] = *change;
        } else {
            changes[change_idx++] = *change;
        }
    }

    qsort(adds, add_count, sizeof(PackageChange), compare_package_changes);
    qsort(changes, change_count, sizeof(PackageChange), compare_package_changes);

    printf("Here is my plan:\n");
    printf("  \n");

    if (add_count > 0) {
        printf("  Add:\n");
        for (int i = 0; i < add_count; i++) {
            PackageChange *change = &adds[i];
            char pkg_name[256];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
            printf("    %-*s    %s\n", max_width, pkg_name, change->new_version);
        }
        printf("  \n");
    }

    if (change_count > 0) {
        printf("  Change:\n");
        for (int i = 0; i < change_count; i++) {
            PackageChange *change = &changes[i];
            char pkg_name[256];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
            printf("    %-*s    %s => %s\n", max_width, pkg_name,
                   change->old_version, change->new_version);
        }
    }

    arena_free(adds);
    arena_free(changes);

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n]: ");
        fflush(stdout);
        
        char response[10];
        if (!fgets(response, sizeof(response), stdin)) {
            fprintf(stderr, "Error reading input\n");
            install_plan_free(out_plan);
            arena_free(author);
            arena_free(name);
            return 1;
        }
        
        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            install_plan_free(out_plan);
            arena_free(author);
            arena_free(name);
            return 0;
        }
    }
    
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        PackageMap *target_map = NULL;
        
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            if (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0) {
                target_map = is_test ? elm_json->dependencies_test_direct : elm_json->dependencies_direct;
            } else {
                target_map = is_test ? elm_json->dependencies_test_indirect : elm_json->dependencies_indirect;
            }
        } else {
            if (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0) {
                target_map = is_test ? elm_json->package_test_dependencies : elm_json->package_dependencies;
            } else {
                target_map = is_test ? elm_json->package_test_dependencies : elm_json->package_dependencies;
            }
        }
        
        if (target_map) {
            package_map_add(target_map, change->author, change->name, change->new_version);
        }
    }
    
    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    printf("Successfully installed %s/%s!\n", author, name);

    install_plan_free(out_plan);
    arena_free(author);
    arena_free(name);
    return 0;
}

int cmd_install(int argc, char *argv[]) {
    bool is_test = false;
    bool major_upgrade = false;
    bool auto_yes = false;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    bool pin_flag = false;
    const char *package_name = NULL;
    const char *major_package_name = NULL;
    const char *from_file_path = NULL;
    const char *from_url = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_install_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cmd_verbose = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cmd_quiet = true;
        } else if (strcmp(argv[i], "--test") == 0) {
            is_test = true;
        } else if (strcmp(argv[i], "--pin") == 0) {
            pin_flag = true;
        } else if (strcmp(argv[i], "--from-file") == 0) {
            // Next argument should be the file path, then package name
            if (i + 2 < argc) {
                i++;
                from_file_path = argv[i];
                i++;
                package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --from-file requires <path> and <package> arguments\n");
                print_install_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "--from-url") == 0) {
            // Next argument should be the URL, then package name
            if (i + 2 < argc) {
                i++;
                from_url = argv[i];
                i++;
                package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --from-url requires <url> and <package> arguments\n");
                print_install_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
            // Next argument should be the package name
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                major_package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --major requires a package name\n");
                print_install_usage();
                return 1;
            }
        } else if (argv[i][0] != '-') {
            // For normal install, it's a package name
            if (package_name) {
                fprintf(stderr, "Error: Multiple package names specified\n");
                return 1;
            }
            package_name = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_install_usage();
            return 1;
        }
    }

    if (major_upgrade) {
        if (!major_package_name) {
            fprintf(stderr, "Error: --major requires a package name\n");
            print_install_usage();
            return 1;
        }
        if (package_name && strcmp(package_name, major_package_name) != 0) {
            fprintf(stderr, "Error: Conflicting package names with --major\n");
            return 1;
        }
        package_name = major_package_name;
    }

    // Validate --from-file and --from-url are mutually exclusive
    if (from_file_path && from_url) {
        fprintf(stderr, "Error: Cannot use both --from-file and --from-url\n");
        return 1;
    }

    // Handle verbose/quiet flags
    // -q takes precedence: if -q is specified, suppress all progress
    // otherwise, if -v is specified OR global verbose is on, show progress
    LogLevel original_level = g_log_level;
    if (cmd_quiet) {
        // Suppress progress logging
        if (g_log_level >= LOG_LEVEL_PROGRESS) {
            log_set_level(LOG_LEVEL_WARN);
        }
    } else if (cmd_verbose && !log_is_progress()) {
        // Enable progress logging only (not debug)
        log_set_level(LOG_LEVEL_PROGRESS);
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

    log_debug("ELM_HOME: %s", env->cache->elm_home);

    log_debug("Reading elm.json");
    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        install_env_free(env);
        return 1;
    }

    int result = 0;

    // Special handling for --from-file and --from-url
    if (from_file_path || from_url) {
        if (!package_name) {
            fprintf(stderr, "Error: Package name required for --from-file or --from-url\n");
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        char *author = NULL;
        char *name = NULL;
        char *version = NULL;
        char *actual_author = NULL;
        char *actual_name = NULL;
        char *actual_version = NULL;
        char temp_dir_buf[1024];
        temp_dir_buf[0] = '\0';

        if (!parse_package_name(package_name, &author, &name)) {
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // For --from-url, download to temp first
        if (from_url) {
            snprintf(temp_dir_buf, sizeof(temp_dir_buf), "/tmp/wrap_temp_%s_%s", author, name);
            mkdir(temp_dir_buf, 0755);

            //REVIEV: magic number.
            char temp_file[1024];
            snprintf(temp_file, sizeof(temp_file), "%s/package.zip", temp_dir_buf);

            printf("Downloading from %s...\n", from_url);
            HttpResult http_result = http_download_file(env->curl_session, from_url, temp_file);
            if (http_result != HTTP_OK) {
                fprintf(stderr, "Error: Failed to download from URL: %s\n", http_result_to_string(http_result));
                arena_free(author);
                arena_free(name);
                elm_json_free(elm_json);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            if (!extract_zip_selective(temp_file, temp_dir_buf)) {
                fprintf(stderr, "Error: Failed to extract archive\n");
                arena_free(author);
                arena_free(name);
                elm_json_free(elm_json);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            unlink(temp_file);

            from_file_path = temp_dir_buf;
        }

        // For --from-file (or after --from-url), check if path exists and read elm.json from it
        struct stat st;
        if (stat(from_file_path, &st) != 0) {
            fprintf(stderr, "Error: Path does not exist: %s\n", from_file_path);
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Try to read elm.json from the source to get actual package info
        char elm_json_path[2048];
        if (S_ISDIR(st.st_mode)) {
            snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", from_file_path);
        } else {
            fprintf(stderr, "Error: --from-file requires a directory path\n");
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Try to find elm.json if not at root (e.g., GitHub archives have a subdirectory)
        if (stat(elm_json_path, &st) != 0) {
            char *found_path = find_package_elm_json(from_file_path);
            if (found_path) {
                snprintf(elm_json_path, sizeof(elm_json_path), "%s", found_path);
                arena_free(found_path);
            } else {
                fprintf(stderr, "Error: Could not find elm.json in %s\n", from_file_path);
                arena_free(author);
                arena_free(name);
                elm_json_free(elm_json);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }
        }

        if (read_package_info_from_elm_json(elm_json_path, &actual_author, &actual_name, &actual_version)) {
            if (strcmp(author, actual_author) != 0 || strcmp(name, actual_name) != 0) {
                printf("Warning: Package name in elm.json (%s/%s) differs from specified name (%s/%s)\n",
                       actual_author, actual_name, author, name);

                if (!auto_yes) {
                    printf("Continue with installation? [Y/n]: ");
                    fflush(stdout);

                    char response[10];
                    if (!fgets(response, sizeof(response), stdin) ||
                        (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
                        printf("Aborted.\n");
                        arena_free(actual_version);
                        arena_free(actual_name);
                        arena_free(actual_author);
                        arena_free(author);
                        arena_free(name);
                        elm_json_free(elm_json);
                        install_env_free(env);
                        log_set_level(original_level);
                        return 0;
                    }
                }
            }

            arena_free(author);
            arena_free(name);
            author = actual_author;
            name = actual_name;
            version = actual_version;
        } else {
            fprintf(stderr, "Error: Could not read package information from %s\n", elm_json_path);
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        Package *existing_pkg = find_existing_package(elm_json, author, name);
        bool is_update = (existing_pkg != NULL);

        printf("Here is my plan:\n");
        printf("  \n");
        if (is_update) {
            printf("  Change:\n");
            printf("    %s/%s    %s => %s\n", author, name, existing_pkg->version, version);
        } else {
            printf("  Add:\n");
            printf("    %s/%s    %s\n", author, name, version);
        }
        printf("  \n");

        if (!auto_yes) {
            printf("\nWould you like me to update your elm.json accordingly? [Y/n]: ");
            fflush(stdout);

            char response[10];
            if (!fgets(response, sizeof(response), stdin) ||
                (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
                printf("Aborted.\n");
                if (version) arena_free(version);
                arena_free(author);
                arena_free(name);
                elm_json_free(elm_json);
                install_env_free(env);
                log_set_level(original_level);
                return 0;
            }
        }

        if (!install_from_file(from_file_path, env, author, name, version)) {
            fprintf(stderr, "Error: Failed to install package from file\n");
            if (version) arena_free(version);
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (pin_flag) {
            // Get package directory (one level up from version dir)
            size_t pkg_dir_len = strlen(env->cache->packages_dir) + strlen(author) + strlen(name) + 3;
            char *pkg_dir = arena_malloc(pkg_dir_len);
            if (pkg_dir) {
                snprintf(pkg_dir, pkg_dir_len, "%s/%s/%s", env->cache->packages_dir, author, name);
                create_pin_file(pkg_dir, version);
                arena_free(pkg_dir);
            }
        }

        if (is_update) {
            arena_free(existing_pkg->version);
            existing_pkg->version = arena_strdup(version);
        } else {
            PackageMap *target_map = NULL;
            if (elm_json->type == ELM_PROJECT_APPLICATION) {
                target_map = is_test ? elm_json->dependencies_test_direct : elm_json->dependencies_direct;
            } else {
                target_map = is_test ? elm_json->package_test_dependencies : elm_json->package_dependencies;
            }

            if (target_map) {
                package_map_add(target_map, author, name, version);
            }
        }

        printf("Saving elm.json...\n");
        if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
            fprintf(stderr, "Error: Failed to write elm.json\n");
            if (version) arena_free(version);
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        printf("Successfully installed %s/%s@%s!\n", author, name, version);

        if (version) arena_free(version);
        arena_free(author);
        arena_free(name);
        result = 0;
    } else if (package_name) {
        result = install_package(package_name, is_test, major_upgrade, auto_yes, elm_json, env);
    } else {
        char *elm_home = arena_strdup(env->cache->elm_home);
        elm_json_free(elm_json);
        install_env_free(env);
        log_set_level(original_level);

        print_install_what(elm_home);
        arena_free(elm_home);
        return 1;
    }

    elm_json_free(elm_json);
    install_env_free(env);

    log_set_level(original_level);

    return result;
}

// Track packages downloaded during cache operation
typedef struct {
    char **packages;      // Array of "author/name@version" strings
    size_t count;
    size_t capacity;
} CacheDownloadList;

static CacheDownloadList* cache_download_list_create(void) {
    CacheDownloadList *list = arena_malloc(sizeof(CacheDownloadList));
    if (!list) return NULL;
    list->capacity = 16;
    list->count = 0;
    list->packages = arena_malloc(sizeof(char *) * list->capacity);
    if (!list->packages) {
        arena_free(list);
        return NULL;
    }
    return list;
}

static void cache_download_list_add(CacheDownloadList *list, const char *author, const char *name, const char *version) {
    if (!list) return;

    // Check if already in list
    for (size_t i = 0; i < list->count; i++) {
        if (list->packages[i]) {
            char check[512];
            snprintf(check, sizeof(check), "%s/%s@%s", author, name, version);
            if (strcmp(list->packages[i], check) == 0) {
                return;  // Already recorded
            }
        }
    }

    // Grow if needed
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        char **new_packages = arena_realloc(list->packages, sizeof(char *) * list->capacity);
        if (!new_packages) return;
        list->packages = new_packages;
    }

    // Add entry
    char *entry = arena_malloc(512);
    if (entry) {
        snprintf(entry, 512, "%s/%s@%s", author, name, version);
        list->packages[list->count++] = entry;
    }
}

static void cache_download_list_free(CacheDownloadList *list) {
    if (!list) return;
    if (list->packages) {
        for (size_t i = 0; i < list->count; i++) {
            if (list->packages[i]) {
                arena_free(list->packages[i]);
            }
        }
        arena_free(list->packages);
    }
    arena_free(list);
}

static bool cache_download_package_recursive(InstallEnv *env, const char *author, const char *name, const char *version, CacheDownloadList *downloaded) {
    if (!env || !author || !name || !version) return false;

    // Check if already fully cached (directory exists AND has src/)
    if (cache_package_fully_downloaded(env->cache, author, name, version)) {
        log_debug("Package %s/%s@%s already cached (verified src/ exists)", author, name, version);
        return true;
    }

    // Check if directory exists but incomplete - log this for debugging
    if (cache_package_exists(env->cache, author, name, version)) {
        log_debug("Package %s/%s@%s directory exists but src/ is missing - re-downloading", author, name, version);
    }

    // Download the package
    log_progress("Downloading %s/%s@%s...", author, name, version);
    if (!install_env_download_package(env, author, name, version)) {
        fprintf(stderr, "Error: Failed to download %s/%s@%s\n", author, name, version);
        return false;
    }

    // Record this download
    cache_download_list_add(downloaded, author, name, version);

    // Read package elm.json to get dependencies
    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        fprintf(stderr, "Error: Failed to get package path for %s/%s@%s\n", author, name, version);
        return false;
    }

    char elm_json_path[2048];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", pkg_path);
    arena_free(pkg_path);

    ElmJson *pkg_elm_json = elm_json_read(elm_json_path);
    if (!pkg_elm_json) {
        log_debug("Could not read elm.json for %s/%s@%s, skipping dependencies", author, name, version);
        return true;
    }

    // Download dependencies recursively
    bool success = true;
    if (pkg_elm_json->type == ELM_PROJECT_PACKAGE && pkg_elm_json->package_dependencies) {
        for (int i = 0; i < pkg_elm_json->package_dependencies->count; i++) {
            Package *dep = &pkg_elm_json->package_dependencies->packages[i];
            if (dep->author && dep->name) {
                // Parse version - it might be a range like "1.0.0 <= v < 2.0.0"
                char *dep_version = NULL;
                if (dep->version && registry_is_version_constraint(dep->version)) {
                    // Resolve constraint to actual version
                    Version resolved;
                    if (registry_resolve_constraint(env->registry, dep->author, dep->name, dep->version, &resolved)) {
                        dep_version = version_to_string(&resolved);
                    }
                } else {
                    dep_version = arena_strdup(dep->version);
                }

                if (dep_version) {
                    if (!cache_download_package_recursive(env, dep->author, dep->name, dep_version, downloaded)) {
                        success = false;
                    }
                    arena_free(dep_version);
                }
            }
        }
    }

    elm_json_free(pkg_elm_json);
    return success;
}

static void print_cache_usage(void) {
    printf("Usage: %s package cache SUBCOMMAND [OPTIONS]\n", program_name);
    printf("\n");
    printf("Cache management commands.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  <PACKAGE> [VERSION]                Download package to cache\n");
    printf("  check <PACKAGE>                    Check cache status for a package\n");
    printf("  full-scan                          Scan entire cache and verify all packages\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package cache elm/html                  # Download latest elm/html\n", program_name);
    printf("  %s package cache elm/html 1.0.0            # Download specific version\n", program_name);
    printf("  %s package cache check elm/html            # Check cache status for elm/html\n", program_name);
    printf("  %s package cache check elm/html --fix-broken # Re-download broken versions\n", program_name);
    printf("  %s package cache full-scan                 # Scan all packages in cache\n", program_name);
    printf("  %s package cache --from-url <url> elm/html # Download from URL to cache\n", program_name);
    printf("  %s package cache --from-file ./pkg elm/html # Download from local file to cache\n", program_name);
    printf("  %s package cache --major elm/html         # Download next major version\n", program_name);
    printf("\n");
    printf("Download Options:\n");
    printf("  <PACKAGE> <VERSION>             # Download specific version (e.g., 1.0.0)\n");
    printf("  --from-file <path> <package>    # Download from local file/directory to cache\n");
    printf("  --from-url <url> <package>      # Download from URL to cache\n");
    printf("  --major <package>               # Download next major version to cache\n");
    printf("  --ignore-hash                   # Skip SHA-1 hash verification\n");
    printf("  -v, --verbose                   # Show progress reports\n");
    printf("  -q, --quiet                     # Suppress progress reports\n");
    printf("  --help                          # Show this help\n");
    printf("\n");
    printf("Check Options:\n");
    printf("  --purge-broken                  # Remove broken directories without re-downloading\n");
    printf("  --fix-broken                    # Try to re-download broken versions\n");
    printf("\n");
    printf("Full-scan Options:\n");
    printf("  -q, --quiet                     # Only show summary counts\n");
    printf("  -v, --verbose                   # Show all issues including missing latest\n");
}

int cmd_cache(int argc, char *argv[]) {
    /* Check for subcommands first */
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        return cmd_cache_check(argc - 1, argv + 1);
    }
    if (argc >= 2 && strcmp(argv[1], "full-scan") == 0) {
        return cmd_cache_full_scan(argc - 1, argv + 1);
    }

    const char *package_arg = NULL;
    const char *version_arg = NULL;
    const char *from_file_path = NULL;
    const char *from_url = NULL;
    const char *major_package_name = NULL;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    bool major_upgrade = false;
    bool ignore_hash = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_cache_usage();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cmd_verbose = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cmd_quiet = true;
        } else if (strcmp(argv[i], "--ignore-hash") == 0) {
            ignore_hash = true;
        } else if (strcmp(argv[i], "--from-file") == 0) {
            if (i + 2 < argc) {
                i++;
                from_file_path = argv[i];
                i++;
                package_arg = argv[i];
            } else {
                fprintf(stderr, "Error: --from-file requires <path> and <package> arguments\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--from-url") == 0) {
            if (i + 2 < argc) {
                i++;
                from_url = argv[i];
                i++;
                package_arg = argv[i];
            } else {
                fprintf(stderr, "Error: --from-url requires <url> and <package> arguments\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                major_package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --major requires a package name\n");
                return 1;
            }
        } else if (argv[i][0] != '-') {
            if (!package_arg) {
                package_arg = argv[i];
            } else if (!version_arg) {
                /* Second positional arg is version (e.g., "1.0.0") */
                version_arg = argv[i];
            } else {
                fprintf(stderr, "Error: Too many positional arguments\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    // Validate arguments
    if (major_upgrade) {
        if (!major_package_name) {
            fprintf(stderr, "Error: --major requires a package name\n");
            return 1;
        }
        if (package_arg && strcmp(package_arg, major_package_name) != 0) {
            fprintf(stderr, "Error: Conflicting package names with --major\n");
            return 1;
        }
        package_arg = major_package_name;
    }

    if (from_file_path && from_url) {
        fprintf(stderr, "Error: Cannot use both --from-file and --from-url\n");
        return 1;
    }

    if (!package_arg) {
        fprintf(stderr, "Error: Package name is required\n");
        fprintf(stderr, "Usage: %s package cache <PACKAGE>\n", program_name);
        return 1;
    }

    // Handle verbose/quiet flags
    LogLevel original_level = g_log_level;
    if (cmd_quiet) {
        if (g_log_level >= LOG_LEVEL_PROGRESS) {
            log_set_level(LOG_LEVEL_WARN);
        }
    } else if (cmd_verbose && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    // Initialize environment
    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        log_set_level(original_level);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    // Set ignore_hash flag if requested
    env->ignore_hash = ignore_hash;

    // Parse package name
    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(package_arg, &author, &name)) {
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    int result = 0;
    char *version = NULL;
    CacheDownloadList *downloaded = NULL;

    // Handle --from-file and --from-url
    if (from_file_path || from_url) {
        char *actual_author = NULL;
        char *actual_name = NULL;
        char *actual_version = NULL;
        char temp_dir_buf[1024];
        temp_dir_buf[0] = '\0';

        // For --from-url, download to temp first
        if (from_url) {
            snprintf(temp_dir_buf, sizeof(temp_dir_buf), "/tmp/wrap_cache_%s_%s", author, name);
            mkdir(temp_dir_buf, 0755);

            char temp_file[1024];
            snprintf(temp_file, sizeof(temp_file), "%s/package.zip", temp_dir_buf);

            printf("Downloading from %s...\n", from_url);
            HttpResult http_result = http_download_file(env->curl_session, from_url, temp_file);
            if (http_result != HTTP_OK) {
                fprintf(stderr, "Error: Failed to download from URL: %s\n", http_result_to_string(http_result));
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            if (!extract_zip_selective(temp_file, temp_dir_buf)) {
                fprintf(stderr, "Error: Failed to extract archive\n");
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            unlink(temp_file);
            from_file_path = temp_dir_buf;
        }

        // For --from-file (or after --from-url), check if path exists
        struct stat st;
        if (stat(from_file_path, &st) != 0) {
            fprintf(stderr, "Error: Path does not exist: %s\n", from_file_path);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Try to read elm.json from the source
        char elm_json_path[2048];
        if (S_ISDIR(st.st_mode)) {
            snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", from_file_path);
        } else {
            fprintf(stderr, "Error: --from-file requires a directory path\n");
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Try to find elm.json if not at root
        if (stat(elm_json_path, &st) != 0) {
            char *found_path = find_package_elm_json(from_file_path);
            if (found_path) {
                snprintf(elm_json_path, sizeof(elm_json_path), "%s", found_path);
                arena_free(found_path);
            } else {
                fprintf(stderr, "Error: Could not find elm.json in %s\n", from_file_path);
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }
        }

        if (!read_package_info_from_elm_json(elm_json_path, &actual_author, &actual_name, &actual_version)) {
            fprintf(stderr, "Error: Could not read package information from %s\n", elm_json_path);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (strcmp(author, actual_author) != 0 || strcmp(name, actual_name) != 0) {
            printf("Warning: Package name in elm.json (%s/%s) differs from specified name (%s/%s)\n",
                   actual_author, actual_name, author, name);
        }

        arena_free(author);
        arena_free(name);
        author = actual_author;
        name = actual_name;
        version = actual_version;

        // Get source directory (might be nested in archive)
        char source_dir[2048];
        char *elm_json_dir = strrchr(elm_json_path, '/');
        if (elm_json_dir) {
            *elm_json_dir = '\0';
            snprintf(source_dir, sizeof(source_dir), "%s", elm_json_path);
            *elm_json_dir = '/';
        } else {
            snprintf(source_dir, sizeof(source_dir), "%s", from_file_path);
        }

        // Copy package to cache
        if (!install_from_file(source_dir, env, author, name, version)) {
            fprintf(stderr, "Error: Failed to copy package to cache\n");
            arena_free(version);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        printf("Successfully cached %s/%s@%s!\n", author, name, version);

        // Clean up temp directory if we used one
        if (from_url && temp_dir_buf[0] != '\0') {
            remove_directory_recursive(temp_dir_buf);
        }

        result = 0;
    } else {
        // Normal package download from registry
        RegistryEntry *registry_entry = registry_find(env->registry, author, name);
        if (!registry_entry) {
            log_error("I cannot find package '%s/%s'", author, name);
            log_error("Make sure the package name is correct");
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Get version
        if (registry_entry->version_count == 0) {
            log_error("Package %s/%s has no versions", author, name);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Use specified version or find appropriate version
        if (version_arg) {
            // User specified a specific version - validate it exists
            Version requested = version_parse(version_arg);
            bool found = false;
            for (size_t i = 0; i < registry_entry->version_count; i++) {
                if (registry_version_compare(&registry_entry->versions[i], &requested) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                log_error("Version %s not found for package %s/%s", version_arg, author, name);
                log_error("Available versions:");
                size_t show_count = registry_entry->version_count < 10 ? registry_entry->version_count : 10;
                for (size_t i = 0; i < show_count; i++) {
                    char *v = version_to_string(&registry_entry->versions[i]);
                    if (v) {
                        log_error("  %s", v);
                        arena_free(v);
                    }
                }
                if (registry_entry->version_count > 10) {
                    log_error("  ... and %zu more", registry_entry->version_count - 10);
                }
                arena_free(author);
                arena_free(name);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }
            version = arena_strdup(version_arg);
        } else if (major_upgrade) {
            // For --major, find next major version
            // Would need current version to determine "next major"
            // For cache, just use latest for now
            version = version_to_string(&registry_entry->versions[0]);
        } else {
            version = version_to_string(&registry_entry->versions[0]);
        }

        if (!version) {
            log_error("Failed to get version for %s/%s", author, name);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Create download tracking list
        downloaded = cache_download_list_create();
        if (!downloaded) {
            log_error("Failed to create download list");
            arena_free(version);
            arena_free(author);
            arena_free(name);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        // Download package and dependencies
        bool success = cache_download_package_recursive(env, author, name, version, downloaded);

        // Report results
        if (success) {
            if (downloaded->count > 0) {
                printf("\nDownloaded %zu package%s to cache:\n", downloaded->count, downloaded->count == 1 ? "" : "s");
                for (size_t i = 0; i < downloaded->count; i++) {
                    printf("  %s\n", downloaded->packages[i]);
                }
            } else {
                printf("Package %s/%s@%s and all dependencies already cached\n", author, name, version);
            }
            result = 0;
        } else {
            result = 1;
        }

        cache_download_list_free(downloaded);
    }

    // Cleanup
    if (version) arena_free(version);
    arena_free(author);
    arena_free(name);
    install_env_free(env);
    log_set_level(original_level);

    return result;
}

static void print_remove_usage(void) {
    printf("Usage: %s package remove <PACKAGE>\n", program_name);
    printf("\n");
    printf("Remove a package from your Elm project.\n");
    printf("\n");
    printf("This will also remove any indirect dependencies that are no longer\n");
    printf("needed by other packages.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package remove elm/html      # Remove elm/html from your project\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
}

int cmd_remove(int argc, char *argv[]) {
    const char *package_name = NULL;
    bool auto_yes = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_remove_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (argv[i][0] != '-') {
            if (package_name) {
                fprintf(stderr, "Error: Multiple package names specified\n");
                return 1;
            }
            package_name = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_remove_usage();
            return 1;
        }
    }

    if (!package_name) {
        fprintf(stderr, "Error: Package name is required\n");
        print_remove_usage();
        return 1;
    }

    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package_name, &author, &name)) {
        return 1;
    }

    log_debug("Removing %s/%s", author, name);

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        arena_free(author);
        arena_free(name);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    log_debug("ELM_HOME: %s", env->cache->elm_home);

    log_debug("Reading elm.json");
    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    SolverState *solver = solver_init(env, false);  // Offline mode - we don't need to download anything
    if (!solver) {
        log_error("Failed to initialize solver");
        elm_json_free(elm_json);
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    InstallPlan *out_plan = NULL;
    SolverResult result = solver_remove_package(solver, elm_json, author, name, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to compute removal plan");

        switch (result) {
            case SOLVER_INVALID_PACKAGE:
                log_error("Package %s/%s is not in your elm.json", author, name);
                break;
            default:
                break;
        }

        elm_json_free(elm_json);
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    qsort(out_plan->changes, out_plan->count, sizeof(PackageChange), compare_package_changes);

    // Calculate max width for formatting
    int max_width = 0;
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        int pkg_len = strlen(change->author) + 1 + strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    printf("Here is my plan:\n");
    printf("  \n");
    printf("  Remove:\n");
    
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        char pkg_name[256];
        snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
        printf("    %-*s    %s\n", max_width, pkg_name, change->old_version);
    }
    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n]: ");
        fflush(stdout);
        
        char response[10];
        if (!fgets(response, sizeof(response), stdin)) {
            fprintf(stderr, "Error reading input\n");
            install_plan_free(out_plan);
            elm_json_free(elm_json);
            install_env_free(env);
            arena_free(author);
            arena_free(name);
            return 1;
        }
        
        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            install_plan_free(out_plan);
            elm_json_free(elm_json);
            install_env_free(env);
            arena_free(author);
            arena_free(name);
            return 0;
        }
    }
    
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            package_map_remove(elm_json->dependencies_direct, change->author, change->name);
            package_map_remove(elm_json->dependencies_indirect, change->author, change->name);
            package_map_remove(elm_json->dependencies_test_direct, change->author, change->name);
            package_map_remove(elm_json->dependencies_test_indirect, change->author, change->name);
        } else {
            package_map_remove(elm_json->package_dependencies, change->author, change->name);
            package_map_remove(elm_json->package_test_dependencies, change->author, change->name);
        }
    }
    
    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        elm_json_free(elm_json);
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    printf("Successfully removed %s/%s!\n", author, name);

    install_plan_free(out_plan);
    elm_json_free(elm_json);
    install_env_free(env);
    arena_free(author);
    arena_free(name);
    return 0;
}

static void print_check_usage(void) {
    printf("Usage: %s package check [elm.json]\n", program_name);
    printf("\n");
    printf("Check for available package upgrades.\n");
    printf("\n");
    printf("This checks packages listed in elm.json against the registry\n");
    printf("in your ELM_HOME cache to find available updates.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help                             # Show this help\n");
}

int cmd_check(int argc, char *argv[]) {
    const char *elm_json_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_check_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            if (elm_json_path) {
                fprintf(stderr, "Error: Multiple elm.json paths specified\n");
                return 1;
            }
            elm_json_path = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_check_usage();
            return 1;
        }
    }

    if (!elm_json_path) {
        elm_json_path = ELM_JSON_PATH;
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

    log_debug("Using registry from: %s", env->cache->registry_path);

    int result = check_all_upgrades(elm_json_path, env->registry);

    install_env_free(env);

    return result;
}

// Helper function to find elm.json in a package directory
// Handles both cases: elm.json at root and elm.json in a subdirectory
static char* find_package_elm_json(const char *pkg_path) {
    // Try direct path first
    size_t direct_len = strlen(pkg_path) + strlen("/elm.json") + 1;
    char *direct_path = arena_malloc(direct_len);
    if (!direct_path) return NULL;
    snprintf(direct_path, direct_len, "%s/elm.json", pkg_path);

    struct stat st;
    if (stat(direct_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return direct_path;
    }
    arena_free(direct_path);

    // Not found at root - look in subdirectories
    DIR *dir = opendir(pkg_path);
    if (!dir) return NULL;

    struct dirent *entry;
    char *found_path = NULL;

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t subdir_len = strlen(pkg_path) + strlen(entry->d_name) + 2;
        char *subdir_path = arena_malloc(subdir_len);
        if (!subdir_path) continue;
        snprintf(subdir_path, subdir_len, "%s/%s", pkg_path, entry->d_name);

        if (stat(subdir_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t elm_json_len = strlen(subdir_path) + strlen("/elm.json") + 1;
            char *elm_json_path = arena_malloc(elm_json_len);
            if (elm_json_path) {
                snprintf(elm_json_path, elm_json_len, "%s/elm.json", subdir_path);
                if (stat(elm_json_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    found_path = elm_json_path;
                    arena_free(subdir_path);
                    break;
                }
                arena_free(elm_json_path);
            }
        }
        arena_free(subdir_path);
    }

    closedir(dir);
    return found_path;
}

static void print_deps_usage(void) {
    printf("Usage: %s package deps <PACKAGE> [VERSION]\n", program_name);
    printf("\n");
    printf("Display all dependencies for a specific package.\n");
    printf("\n");
    printf("Version resolution:\n");
    printf("  - If package is in elm.json: uses that version\n");
    printf("  - If not in elm.json and no VERSION specified: uses latest from registry\n");
    printf("  - If VERSION specified: uses that specific version\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package deps elm/http         # Show dependencies for elm/http\n", program_name);
    printf("  %s package deps elm/http 2.0.0   # Show dependencies for elm/http 2.0.0\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --help                             # Show this help\n");
}

static void print_info_usage(void) {
    printf("Usage: %s package info\n", program_name);
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
    printf("Example:\n");
    printf("  %s package info                  # Show general package info\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --help                             # Show this help\n");
}

static bool package_depends_on(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                               const char *target_author, const char *target_name,
                               InstallEnv *env) {
    char *pkg_path = cache_get_package_path(env->cache, pkg_author, pkg_name, pkg_version);
    if (!pkg_path) {
        return false;
    }

    char *elm_json_path = find_package_elm_json(pkg_path);
    ElmJson *pkg_elm_json = NULL;

    if (elm_json_path) {
        pkg_elm_json = elm_json_read(elm_json_path);
        arena_free(elm_json_path);
    }

    if (!pkg_elm_json) {
        if (cache_download_package_with_env(env, pkg_author, pkg_name, pkg_version)) {
            elm_json_path = find_package_elm_json(pkg_path);
            if (elm_json_path) {
                pkg_elm_json = elm_json_read(elm_json_path);
                arena_free(elm_json_path);
            }
        }
    }

    arena_free(pkg_path);

    if (!pkg_elm_json) {
        return false;
    }

    bool depends = false;

    if (pkg_elm_json->package_dependencies) {
        Package *dep = package_map_find(pkg_elm_json->package_dependencies, target_author, target_name);
        if (dep) {
            depends = true;
        }
    }

    if (!depends && pkg_elm_json->package_test_dependencies) {
        Package *dep = package_map_find(pkg_elm_json->package_test_dependencies, target_author, target_name);
        if (dep) {
            depends = true;
        }
    }

    elm_json_free(pkg_elm_json);
    return depends;
}

static int show_package_dependencies(const char *author, const char *name, const char *version, InstallEnv *env) {
    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        log_error("Failed to get package path");
        return 1;
    }

    char *elm_json_path = find_package_elm_json(pkg_path);

    ElmJson *elm_json = NULL;
    if (elm_json_path) {
        elm_json = elm_json_read(elm_json_path);
    }

    if (!elm_json) {
        log_debug("Package not in cache, attempting download");
        if (!cache_download_package_with_env(env, author, name, version)) {
            log_error("Failed to download package %s/%s@%s", author, name, version);
            if (elm_json_path) arena_free(elm_json_path);
            arena_free(pkg_path);
            return 1;
        }

        if (elm_json_path) arena_free(elm_json_path);
        elm_json_path = find_package_elm_json(pkg_path);

        if (elm_json_path) {
            elm_json = elm_json_read(elm_json_path);
        }

        if (!elm_json) {
            log_error("Failed to read elm.json for %s/%s@%s", author, name, version);
            if (elm_json_path) arena_free(elm_json_path);
            arena_free(pkg_path);
            return 1;
        }
    }

    if (elm_json_path) arena_free(elm_json_path);
    arena_free(pkg_path);

    printf("\n");
    printf("Package: %s/%s @ %s\n", author, name, version);
    printf("========================================\n\n");

    if (elm_json->type == ELM_PROJECT_PACKAGE && elm_json->package_dependencies) {
        PackageMap *deps = elm_json->package_dependencies;

        if (deps->count == 0) {
            printf("No dependencies\n");
        } else {
            int max_width = 0;
            for (int i = 0; i < deps->count; i++) {
                Package *pkg = &deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            if (elm_json->package_test_dependencies && elm_json->package_test_dependencies->count > 0) {
                PackageMap *test_deps = elm_json->package_test_dependencies;
                for (int i = 0; i < test_deps->count; i++) {
                    Package *pkg = &test_deps->packages[i];
                    int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                    if (pkg_len > max_width) max_width = pkg_len;
                }
            }

            printf("Dependencies (%d):\n", deps->count);
            for (int i = 0; i < deps->count; i++) {
                Package *pkg = &deps->packages[i];
                char pkg_name[256];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s    %s\n", max_width, pkg_name, pkg->version);
            }
        }

        if (elm_json->package_test_dependencies && elm_json->package_test_dependencies->count > 0) {
            PackageMap *test_deps = elm_json->package_test_dependencies;

            int max_width = 0;
            for (int i = 0; i < test_deps->count; i++) {
                Package *pkg = &test_deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            if (elm_json->package_dependencies) {
                for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                    Package *pkg = &elm_json->package_dependencies->packages[i];
                    int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                    if (pkg_len > max_width) max_width = pkg_len;
                }
            }

            printf("\nTest Dependencies (%d):\n", test_deps->count);
            for (int i = 0; i < test_deps->count; i++) {
                Package *pkg = &test_deps->packages[i];
                char pkg_name[256];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s    %s\n", max_width, pkg_name, pkg->version);
            }
        }
    } else {
        printf("(Not a package - this is an application)\n");
    }

    ElmJson *current_elm_json = elm_json_read(ELM_JSON_PATH);
    if (current_elm_json) {
        PackageMap *all_deps = package_map_create();

        if (current_elm_json->dependencies_direct) {
            for (int i = 0; i < current_elm_json->dependencies_direct->count; i++) {
                Package *pkg = &current_elm_json->dependencies_direct->packages[i];
                package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (current_elm_json->dependencies_indirect) {
            for (int i = 0; i < current_elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &current_elm_json->dependencies_indirect->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->dependencies_test_direct) {
            for (int i = 0; i < current_elm_json->dependencies_test_direct->count; i++) {
                Package *pkg = &current_elm_json->dependencies_test_direct->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->dependencies_test_indirect) {
            for (int i = 0; i < current_elm_json->dependencies_test_indirect->count; i++) {
                Package *pkg = &current_elm_json->dependencies_test_indirect->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->package_dependencies) {
            for (int i = 0; i < current_elm_json->package_dependencies->count; i++) {
                Package *pkg = &current_elm_json->package_dependencies->packages[i];
                package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (current_elm_json->package_test_dependencies) {
            for (int i = 0; i < current_elm_json->package_test_dependencies->count; i++) {
                Package *pkg = &current_elm_json->package_test_dependencies->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        PackageMap *reverse_deps = package_map_create();

        for (int i = 0; i < all_deps->count; i++) {
            Package *pkg = &all_deps->packages[i];

            // Skip the target package itself
            if (strcmp(pkg->author, author) == 0 && strcmp(pkg->name, name) == 0) {
                continue;
            }

            if (package_depends_on(pkg->author, pkg->name, pkg->version, author, name, env)) {
                package_map_add(reverse_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (reverse_deps->count > 0) {
            // Calculate max width for aligned output
            int max_width = 0;
            for (int i = 0; i < reverse_deps->count; i++) {
                Package *pkg = &reverse_deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            printf("\nPackages in elm.json that depend on %s/%s (%d):\n", author, name, reverse_deps->count);
            for (int i = 0; i < reverse_deps->count; i++) {
                Package *pkg = &reverse_deps->packages[i];
                char pkg_name[256];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                printf("  %-*s    %s\n", max_width, pkg_name, pkg->version);
            }
        }

        package_map_free(reverse_deps);
        package_map_free(all_deps);
        elm_json_free(current_elm_json);
    }

    printf("\n");
    elm_json_free(elm_json);
    return 0;
}

int cmd_deps(int argc, char *argv[]) {
    const char *package_arg = NULL;
    const char *version_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_deps_usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_deps_usage();
            return 1;
        } else {
            if (!package_arg) {
                package_arg = argv[i];
            } else if (!version_arg) {
                version_arg = argv[i];
            } else {
                fprintf(stderr, "Error: Too many arguments\n");
                print_deps_usage();
                return 1;
            }
        }
    }

    if (!package_arg) {
        fprintf(stderr, "Error: Package name is required\n");
        print_deps_usage();
        return 1;
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

    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package_arg, &author, &name)) {
        install_env_free(env);
        return 1;
    }

    RegistryEntry *registry_entry = registry_find(env->registry, author, name);
    if (!registry_entry) {
        log_error("I cannot find package '%s/%s'", author, name);
        log_error("Make sure the package name is correct");
        arena_free(author);
        arena_free(name);
        install_env_free(env);
        return 1;
    }

    const char *version_to_use = NULL;
    bool version_found = false;

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
            log_error("Version %s not found for package %s/%s", version_arg, author, name);
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
            install_env_free(env);
            return 1;
        }
    } else {
        ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
        if (elm_json) {
            Package *existing_pkg = find_existing_package(elm_json, author, name);
            if (existing_pkg && existing_pkg->version) {
                version_to_use = existing_pkg->version;
                version_found = true;
                log_debug("Using version %s from elm.json", version_to_use);
            }
            elm_json_free(elm_json);
        }

        if (!version_found && registry_entry->version_count > 0) {
            // Versions are stored newest first
            char *latest = version_to_string(&registry_entry->versions[0]);
            if (latest) {
                version_to_use = latest;
                version_found = true;
                log_debug("Using latest version %s from registry", version_to_use);
            }
        }
    }

    if (!version_found || !version_to_use) {
        log_error("Could not determine version for %s/%s", author, name);
        arena_free(author);
        arena_free(name);
        install_env_free(env);
        return 1;
    }

    int result = show_package_dependencies(author, name, version_to_use, env);

    if (version_to_use != version_arg && version_found && !version_arg) {
        ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
        if (!elm_json || !find_existing_package(elm_json, author, name)) {
            arena_free((char*)version_to_use);
        }
        if (elm_json) {
            elm_json_free(elm_json);
        }
    }

    arena_free(author);
    arena_free(name);
    install_env_free(env);
    return result;
}

int cmd_info(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_info_usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_info_usage();
            return 1;
        } else {
            fprintf(stderr, "Error: Unexpected argument: %s\n", argv[i]);
            fprintf(stderr, "The 'info' command no longer accepts package arguments.\n");
            fprintf(stderr, "Use 'wrap package deps <PACKAGE>' instead.\n");
            return 1;
        }
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

    printf("\n");
    printf("Package Management Information\n");
    printf("===============================\n\n");

    printf("ELM_HOME: %s\n", env->cache->elm_home);

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

    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
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

        printf("\nInstalled Package Versions:\n");
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
                Package *pkg = &elm_json->dependencies_direct->packages[i];
                printf("  %s/%s  %s\n", pkg->author, pkg->name, pkg->version);
            }
            for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &elm_json->dependencies_indirect->packages[i];
                printf("  %s/%s  %s (indirect)\n", pkg->author, pkg->name, pkg->version);
            }
        } else {
            for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                Package *pkg = &elm_json->package_dependencies->packages[i];
                printf("  %s/%s  %s\n", pkg->author, pkg->name, pkg->version);
            }
        }

        printf("\n");
        check_all_upgrades(ELM_JSON_PATH, env->registry);

        elm_json_free(elm_json);
    } else {
        printf("\n(Not in an Elm project directory)\n");
    }

    printf("\n");

    install_env_free(env);

    return 0;
}

static int upgrade_single_package(const char *package, ElmJson *elm_json, InstallEnv *env, bool major_upgrade, bool major_ignore_test, bool auto_yes) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package, &author, &name)) {
        return 1;
    }

    log_debug("Upgrading %s/%s%s%s", author, name, major_upgrade ? " (major allowed)" : "", major_ignore_test ? " (ignoring test deps)" : "");

    Package *existing_pkg = find_existing_package(elm_json, author, name);
    if (!existing_pkg) {
        fprintf(stderr, "Error: Package %s/%s is not installed\n", author, name);
        fprintf(stderr, "Run '%s package check' to see available upgrades\n", program_name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    RegistryEntry *registry_entry = registry_find(env->registry, author, name);
    if (!registry_entry) {
        log_error("I cannot find package '%s/%s' in registry", author, name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    const char *latest_version = NULL;
    if (major_upgrade) {
        // Latest version overall is  the first in the list
        if (registry_entry->version_count > 0) {
            latest_version = version_to_string(&registry_entry->versions[0]);
        }
    } else {
        // Latest version within same major
        int current_major, current_minor, current_patch;
        parse_version(existing_pkg->version, &current_major, &current_minor, &current_patch);

        for (size_t i = 0; i < registry_entry->version_count; i++) {
            Version *v = &registry_entry->versions[i];
            if (v->major == current_major) {
                latest_version = version_to_string(v);
                break;
            }
        }
    }

    if (!latest_version) {
        printf("No %s upgrades available for %s/%s\n",
               major_upgrade ? "major" : "minor", author, name);
        arena_free(author);
        arena_free(name);
        return 0;
    }

    if (strcmp(existing_pkg->version, latest_version) == 0) {
        printf("Package %s/%s is already at the latest %s version (%s)\n",
               author, name, major_upgrade ? "major" : "minor", latest_version);
        arena_free((char*)latest_version);
        arena_free(author);
        arena_free(name);
        return 0;
    }

    // For major upgrades, check if any installed packages depend on the current version
    if (major_upgrade) {
        int current_major, current_minor, current_patch;
        int new_major, new_minor, new_patch;
        parse_version(existing_pkg->version, &current_major, &current_minor, &current_patch);
        parse_version(latest_version, &new_major, &new_minor, &new_patch);

        // Only check if we're actually crossing a major version boundary
        if (new_major != current_major) {
            PackageMap *all_deps = package_map_create();

            if (elm_json->dependencies_direct) {
                for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
                    Package *pkg = &elm_json->dependencies_direct->packages[i];
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }

            if (elm_json->dependencies_indirect) {
                for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
                    Package *pkg = &elm_json->dependencies_indirect->packages[i];
                    if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                        package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                    }
                }
            }

            if (elm_json->dependencies_test_direct) {
                for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
                    Package *pkg = &elm_json->dependencies_test_direct->packages[i];
                    if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                        package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                    }
                }
            }

            if (elm_json->dependencies_test_indirect) {
                for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
                    Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
                    if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                        package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                    }
                }
            }

            if (elm_json->package_dependencies) {
                for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                    Package *pkg = &elm_json->package_dependencies->packages[i];
                    if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                        package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                    }
                }
            }

            if (elm_json->package_test_dependencies) {
                for (int i = 0; i < elm_json->package_test_dependencies->count; i++) {
                    Package *pkg = &elm_json->package_test_dependencies->packages[i];
                    if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                        package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                    }
                }
            }

            PackageMap *reverse_deps = package_map_create();
            PackageMap *reverse_deps_test = package_map_create();

            for (int i = 0; i < all_deps->count; i++) {
                Package *pkg = &all_deps->packages[i];

                if (strcmp(pkg->author, author) == 0 && strcmp(pkg->name, name) == 0) {
                    continue;
                }

                if (package_depends_on(pkg->author, pkg->name, pkg->version, author, name, env)) {
                    bool is_test_dep = false;
                    if (elm_json->dependencies_test_direct && package_map_find(elm_json->dependencies_test_direct, pkg->author, pkg->name)) {
                        is_test_dep = true;
                    } else if (elm_json->dependencies_test_indirect && package_map_find(elm_json->dependencies_test_indirect, pkg->author, pkg->name)) {
                        is_test_dep = true;
                    } else if (elm_json->package_test_dependencies && package_map_find(elm_json->package_test_dependencies, pkg->author, pkg->name)) {
                        is_test_dep = true;
                    }

                    if (is_test_dep) {
                        package_map_add(reverse_deps_test, pkg->author, pkg->name, pkg->version);
                    } else {
                        package_map_add(reverse_deps, pkg->author, pkg->name, pkg->version);
                    }
                }
            }

            // If there are reverse dependencies, check if upgrades are available
            // When major_ignore_test is set, only check non-test dependencies for blocking
            int total_reverse_deps = reverse_deps->count + reverse_deps_test->count;

            if (total_reverse_deps > 0) {
                printf("\nWarning: The following packages depend on %s/%s %d.x.x:\n", author, name, current_major);

                bool has_blocking_deps = false;
                bool has_test_blocking_deps = false;
                PackageMap *blocking_deps = package_map_create();
                PackageMap *blocking_test_deps = package_map_create();

                for (int i = 0; i < reverse_deps->count; i++) {
                    Package *pkg = &reverse_deps->packages[i];

                    RegistryEntry *dep_registry = registry_find(env->registry, pkg->author, pkg->name);
                    bool has_upgrade = false;

                    if (dep_registry && dep_registry->version_count > 0) {
                        const char *newest = version_to_string(&dep_registry->versions[0]);
                        if (strcmp(newest, pkg->version) != 0) {
                            has_upgrade = true;
                        }
                        arena_free((char*)newest);
                    }

                    if (has_upgrade) {
                        printf("  %s/%s %s (upgrade may be available)\n",
                               pkg->author, pkg->name, pkg->version);
                    } else {
                        printf("  %s/%s %s (no upgrade available)\n",
                               pkg->author, pkg->name, pkg->version);
                        has_blocking_deps = true;
                        package_map_add(blocking_deps, pkg->author, pkg->name, pkg->version);
                    }
                }

                for (int i = 0; i < reverse_deps_test->count; i++) {
                    Package *pkg = &reverse_deps_test->packages[i];

                    RegistryEntry *dep_registry = registry_find(env->registry, pkg->author, pkg->name);
                    bool has_upgrade = false;

                    if (dep_registry && dep_registry->version_count > 0) {
                        const char *newest = version_to_string(&dep_registry->versions[0]);
                        if (strcmp(newest, pkg->version) != 0) {
                            has_upgrade = true;
                        }
                        arena_free((char*)newest);
                    }

                    if (has_upgrade) {
                        printf("  %s/%s %s [test] (upgrade may be available)\n",
                               pkg->author, pkg->name, pkg->version);
                    } else {
                        printf("  %s/%s %s [test] (no upgrade available)\n",
                               pkg->author, pkg->name, pkg->version);
                        has_test_blocking_deps = true;
                        package_map_add(blocking_test_deps, pkg->author, pkg->name, pkg->version);
                    }
                }

                printf("\n");

                if (has_blocking_deps) {
                    fprintf(stderr, "Error: Cannot upgrade %s/%s to %d.x.x because the following packages\n",
                            author, name, new_major);
                    fprintf(stderr, "depend on version %d.x.x and have no newer versions available:\n\n",
                            current_major);

                    for (int i = 0; i < blocking_deps->count; i++) {
                        Package *pkg = &blocking_deps->packages[i];
                        fprintf(stderr, "  %s/%s %s\n", pkg->author, pkg->name, pkg->version);
                    }

                    fprintf(stderr, "\nTo proceed, you must first remove these packages from your elm.json\n");
                    fprintf(stderr, "or find compatible versions that support %s/%s %d.x.x\n",
                            author, name, new_major);

                    package_map_free(blocking_test_deps);
                    package_map_free(blocking_deps);
                    package_map_free(reverse_deps_test);
                    package_map_free(reverse_deps);
                    package_map_free(all_deps);
                    arena_free((char*)latest_version);
                    arena_free(author);
                    arena_free(name);
                    return 1;
                }

                if (has_test_blocking_deps && major_ignore_test) {
                    printf("Warning: The following test dependencies would normally block this upgrade:\n\n");

                    for (int i = 0; i < blocking_test_deps->count; i++) {
                        Package *pkg = &blocking_test_deps->packages[i];
                        printf("  %s/%s %s [test]\n", pkg->author, pkg->name, pkg->version);
                    }

                    printf("\nProceeding with major upgrade because --major-ignore-test was specified.\n");
                    printf("Note: You may need to update or remove these test dependencies manually.\n\n");
                } else if (has_test_blocking_deps && !major_ignore_test) {
                    // Normal --major mode: test dependencies also block
                    // TODO: test dependencies should not block!
                    fprintf(stderr, "Error: Cannot upgrade %s/%s to %d.x.x because the following test dependencies\n",
                            author, name, new_major);
                    fprintf(stderr, "depend on version %d.x.x and have no newer versions available:\n\n",
                            current_major);

                    for (int i = 0; i < blocking_test_deps->count; i++) {
                        Package *pkg = &blocking_test_deps->packages[i];
                        fprintf(stderr, "  %s/%s %s [test]\n", pkg->author, pkg->name, pkg->version);
                    }

                    fprintf(stderr, "\nTo proceed, you can either:\n");
                    fprintf(stderr, "  - Remove these test packages from your elm.json\n");
                    fprintf(stderr, "  - Find compatible versions that support %s/%s %d.x.x\n",
                            author, name, new_major);
                    fprintf(stderr, "  - Use --major-ignore-test to ignore test dependency conflicts\n");

                    package_map_free(blocking_test_deps);
                    package_map_free(blocking_deps);
                    package_map_free(reverse_deps_test);
                    package_map_free(reverse_deps);
                    package_map_free(all_deps);
                    arena_free((char*)latest_version);
                    arena_free(author);
                    arena_free(name);
                    return 1;
                }

                package_map_free(blocking_test_deps);
                package_map_free(blocking_deps);
            }

            package_map_free(reverse_deps_test);
            package_map_free(reverse_deps);
            package_map_free(all_deps);
        }
    }

    log_debug("Resolving dependencies for %s/%s upgrade to %s", author, name, latest_version);

    SolverState *solver = solver_init(env, true);
    if (!solver) {
        log_error("Failed to initialize solver");
        arena_free((char*)latest_version);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    InstallPlan *out_plan = NULL;
    bool is_test = (package_map_find(elm_json->dependencies_test_direct, author, name) != NULL ||
                    package_map_find(elm_json->dependencies_test_indirect, author, name) != NULL ||
                    package_map_find(elm_json->package_test_dependencies, author, name) != NULL);

    SolverResult result = solver_add_package(solver, elm_json, author, name, is_test, major_upgrade, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to resolve dependencies");

        switch (result) {
            case SOLVER_NO_SOLUTION:
                log_error("No compatible version found for %s/%s", author, name);
                break;
            case SOLVER_NO_OFFLINE_SOLUTION:
                log_error("Cannot solve offline (no cached registry)");
                break;
            case SOLVER_NETWORK_ERROR:
                log_error("Network error while downloading packages");
                break;
            case SOLVER_INVALID_PACKAGE:
                log_error("Invalid package specification");
                break;
            default:
                break;
        }

        arena_free((char*)latest_version);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    int add_count = 0;
    int change_count = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        if (!change->old_version) {
            add_count++;
        } else {
            change_count++;
        }
    }

    PackageChange *adds = arena_malloc(sizeof(PackageChange) * add_count);
    PackageChange *changes = arena_malloc(sizeof(PackageChange) * change_count);

    int add_idx = 0;
    int change_idx = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        if (!change->old_version) {
            adds[add_idx++] = *change;
        } else {
            changes[change_idx++] = *change;
        }
    }

    qsort(adds, add_count, sizeof(PackageChange), compare_package_changes);
    qsort(changes, change_count, sizeof(PackageChange), compare_package_changes);

    int max_width = 0;
    for (int i = 0; i < add_count; i++) {
        PackageChange *change = &adds[i];
        int pkg_len = strlen(change->author) + 1 + strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }
    for (int i = 0; i < change_count; i++) {
        PackageChange *change = &changes[i];
        int pkg_len = strlen(change->author) + 1 + strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    printf("Here is my plan:\n");
    printf("  \n");

    if (add_count > 0) {
        printf("  Add:\n");
        for (int i = 0; i < add_count; i++) {
            PackageChange *change = &adds[i];
            char pkg_name[256];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
            printf("    %-*s    %s\n", max_width, pkg_name, change->new_version);
        }
        printf("  \n");
    }

    if (change_count > 0) {
        printf("  Change:\n");
        for (int i = 0; i < change_count; i++) {
            PackageChange *change = &changes[i];
            char pkg_name[256];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
            printf("    %-*s    %s => %s\n", max_width, pkg_name,
                   change->old_version, change->new_version);
        }
    }

    arena_free(adds);
    arena_free(changes);

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n]: ");
        fflush(stdout);

        char response[10];
        if (!fgets(response, sizeof(response), stdin)) {
            fprintf(stderr, "Error reading input\n");
            install_plan_free(out_plan);
            arena_free((char*)latest_version);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            install_plan_free(out_plan);
            arena_free((char*)latest_version);
            arena_free(author);
            arena_free(name);
            return 0;
        }
    }

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        PackageMap *target_map = NULL;

        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            if (change->old_version) {
                // This is a version change - update in place
                Package *pkg = package_map_find(elm_json->dependencies_direct, change->author, change->name);
                if (pkg) {
                    arena_free(pkg->version);
                    pkg->version = arena_strdup(change->new_version);
                    continue;
                }

                pkg = package_map_find(elm_json->dependencies_indirect, change->author, change->name);
                if (pkg) {
                    arena_free(pkg->version);
                    pkg->version = arena_strdup(change->new_version);
                    continue;
                }

                pkg = package_map_find(elm_json->dependencies_test_direct, change->author, change->name);
                if (pkg) {
                    arena_free(pkg->version);
                    pkg->version = arena_strdup(change->new_version);
                    continue;
                }

                pkg = package_map_find(elm_json->dependencies_test_indirect, change->author, change->name);
                if (pkg) {
                    arena_free(pkg->version);
                    pkg->version = arena_strdup(change->new_version);
                    continue;
                }
            } else {
                // This is a new package - add to appropriate map
                if (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0) {
                    // Requested package goes to direct
                    target_map = is_test ? elm_json->dependencies_test_direct : elm_json->dependencies_direct;
                } else {
                    // Others to indirect
                    target_map = is_test ? elm_json->dependencies_test_indirect : elm_json->dependencies_indirect;
                }
                package_map_add(target_map, change->author, change->name, change->new_version);
            }
        } else {
            if (change->old_version) {
                // This is a version change - update in place
                Package *pkg = package_map_find(elm_json->package_dependencies, change->author, change->name);
                if (pkg) {
                    arena_free(pkg->version);
                    pkg->version = arena_strdup(change->new_version);
                    continue;
                }

                pkg = package_map_find(elm_json->package_test_dependencies, change->author, change->name);
                if (pkg) {
                    arena_free(pkg->version);
                    pkg->version = arena_strdup(change->new_version);
                    continue;
                }
            } else {
                // This is a new package - add to appropriate map
                target_map = is_test ? elm_json->package_test_dependencies : elm_json->package_dependencies;
                package_map_add(target_map, change->author, change->name, change->new_version);
            }
        }
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        arena_free((char*)latest_version);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    printf("Successfully upgraded %s/%s!\n", author, name);

    install_plan_free(out_plan);
    arena_free((char*)latest_version);
    arena_free(author);
    arena_free(name);
    return 0;
}

static int upgrade_all_packages(ElmJson *elm_json, InstallEnv *env, bool major_upgrade, bool major_ignore_test, bool auto_yes) {
    // Note: major_ignore_test is currently unused in upgrade_all because the bulk upgrade
    // path uses solver_upgrade_all which doesn't perform the same blocking dependency checks
    // as upgrade_single_package. This parameter is accepted for API consistency.
    (void)major_ignore_test;  // Suppress unused parameter warning

    log_debug("Upgrading all packages%s", major_upgrade ? " (major allowed)" : "");

    SolverState *solver = solver_init(env, true);
    if (!solver) {
        log_error("Failed to initialize solver");
        return 1;
    }

    InstallPlan *out_plan = NULL;
    SolverResult result = solver_upgrade_all(solver, elm_json, major_upgrade, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to compute upgrade plan");

        switch (result) {
            case SOLVER_NO_SOLUTION:
                log_error("No solution found for upgrades");
                break;
            case SOLVER_NO_OFFLINE_SOLUTION:
                log_error("Cannot solve offline (no cached registry)");
                break;
            case SOLVER_NETWORK_ERROR:
                log_error("Network error while downloading packages");
                break;
            case SOLVER_INVALID_PACKAGE:
                log_error("Invalid package specification");
                break;
            default:
                break;
        }

        return 1;
    }

    if (out_plan->count == 0) {
        printf("No upgrades available. All packages are at their latest %s version.\n",
               major_upgrade ? "major" : "minor");
        install_plan_free(out_plan);
        return 0;
    }

    qsort(out_plan->changes, out_plan->count, sizeof(PackageChange), compare_package_changes);

    int max_width = 0;
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        int pkg_len = strlen(change->author) + 1 + strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    printf("Here is my plan:\n");
    printf("  \n");
    printf("  Change:\n");

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        char pkg_name[256];
        snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
        printf("    %-*s    %s => %s\n", max_width, pkg_name,
               change->old_version, change->new_version);
    }
    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n]: ");
        fflush(stdout);

        char response[10];
        if (!fgets(response, sizeof(response), stdin)) {
            fprintf(stderr, "Error reading input\n");
            install_plan_free(out_plan);
            return 1;
        }

        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            install_plan_free(out_plan);
            return 0;
        }
    }

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];

        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            Package *pkg = package_map_find(elm_json->dependencies_direct, change->author, change->name);
            if (pkg) {
                arena_free(pkg->version);
                pkg->version = arena_strdup(change->new_version);
                continue;
            }

            pkg = package_map_find(elm_json->dependencies_indirect, change->author, change->name);
            if (pkg) {
                arena_free(pkg->version);
                pkg->version = arena_strdup(change->new_version);
                continue;
            }

            pkg = package_map_find(elm_json->dependencies_test_direct, change->author, change->name);
            if (pkg) {
                arena_free(pkg->version);
                pkg->version = arena_strdup(change->new_version);
                continue;
            }

            pkg = package_map_find(elm_json->dependencies_test_indirect, change->author, change->name);
            if (pkg) {
                arena_free(pkg->version);
                pkg->version = arena_strdup(change->new_version);
                continue;
            }
        } else {
            Package *pkg = package_map_find(elm_json->package_dependencies, change->author, change->name);
            if (pkg) {
                arena_free(pkg->version);
                pkg->version = arena_strdup(change->new_version);
                continue;
            }

            pkg = package_map_find(elm_json->package_test_dependencies, change->author, change->name);
            if (pkg) {
                arena_free(pkg->version);
                pkg->version = arena_strdup(change->new_version);
                continue;
            }
        }

        log_error("Package %s/%s not found in elm.json (this should not happen)",
                  change->author, change->name);
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        return 1;
    }

    printf("Successfully upgraded %d package(s)!\n", out_plan->count);

    install_plan_free(out_plan);
    return 0;
}

static void print_upgrade_usage(void) {
    printf("Usage: %s package upgrade [PACKAGE|all]\n", program_name);
    printf("\n");
    printf("Upgrade packages to their latest available versions.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package upgrade                        # Upgrade all packages to latest minor versions\n", program_name);
    printf("  %s package upgrade all                    # Same as above\n", program_name);
    printf("  %s package upgrade elm/html               # Upgrade elm/html to latest minor version\n", program_name);
    printf("  %s package upgrade --major elm/html       # Upgrade elm/html to latest major version\n", program_name);
    printf("  %s package upgrade --major all            # Upgrade all packages to latest major versions\n", program_name);
    printf("  %s package upgrade --major-ignore-test elm/html # Major upgrade, ignore test deps\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --major                              # Allow major version upgrades\n");
    printf("  --major-ignore-test                  # Allow major upgrades, ignore test dependency conflicts\n");
    printf("  -y, --yes                            # Automatically confirm changes\n");
    printf("  -v, --verbose                        # Show progress reports (registry, connectivity)\n");
    printf("  -q, --quiet                          # Suppress progress reports\n");
    printf("  --help                               # Show this help\n");
}

int cmd_upgrade(int argc, char *argv[]) {
    bool major_upgrade = false;
    bool major_ignore_test = false;
    bool auto_yes = false;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    const char *package_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_upgrade_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cmd_verbose = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cmd_quiet = true;
        } else if (strcmp(argv[i], "--major-ignore-test") == 0) {
            major_upgrade = true;
            major_ignore_test = true;
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
        } else if (argv[i][0] != '-') {
            if (package_name) {
                fprintf(stderr, "Error: Multiple package names specified\n");
                return 1;
            }
            package_name = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_upgrade_usage();
            return 1;
        }
    }

    // Default to "all" if no package specified
    bool upgrade_all = false;
    if (!package_name || strcmp(package_name, "all") == 0) {
        upgrade_all = true;
    }

    // Handle verbose/quiet flags
    // -q takes precedence: if -q is specified, suppress all progress
    // otherwise, if -v is specified OR global verbose is on, show progress
    LogLevel original_level = g_log_level;
    if (cmd_quiet) {
        if (g_log_level >= LOG_LEVEL_PROGRESS) {
            log_set_level(LOG_LEVEL_WARN);
        }
    } else if (cmd_verbose && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        log_set_level(original_level);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    int result = 0;

    if (upgrade_all) {
        result = upgrade_all_packages(elm_json, env, major_upgrade, major_ignore_test, auto_yes);
    } else {
        result = upgrade_single_package(package_name, elm_json, env, major_upgrade, major_ignore_test, auto_yes);
    }

    elm_json_free(elm_json);
    install_env_free(env);

    log_set_level(original_level);

    return result;
}
