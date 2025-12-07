#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../cache.h"
#include "../../solver.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../http_client.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANSI_DULL_CYAN "\033[36m"
#define ANSI_DULL_YELLOW "\033[33m"
#define ANSI_RESET "\033[0m"

/* Helper functions for placing package changes into the correct dependency map */
static PackageMap* find_existing_app_map(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || elm_json->type != ELM_PROJECT_APPLICATION) {
        return NULL;
    }

    if (package_map_find(elm_json->dependencies_direct, author, name)) {
        return elm_json->dependencies_direct;
    }
    if (package_map_find(elm_json->dependencies_indirect, author, name)) {
        return elm_json->dependencies_indirect;
    }
    if (package_map_find(elm_json->dependencies_test_direct, author, name)) {
        return elm_json->dependencies_test_direct;
    }
    if (package_map_find(elm_json->dependencies_test_indirect, author, name)) {
        return elm_json->dependencies_test_indirect;
    }

    return NULL;
}

static void remove_from_all_app_maps(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || elm_json->type != ELM_PROJECT_APPLICATION) {
        return;
    }

    package_map_remove(elm_json->dependencies_direct, author, name);
    package_map_remove(elm_json->dependencies_indirect, author, name);
    package_map_remove(elm_json->dependencies_test_direct, author, name);
    package_map_remove(elm_json->dependencies_test_indirect, author, name);
}

static PackageMap* find_existing_package_map(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || elm_json->type != ELM_PROJECT_PACKAGE) {
        return NULL;
    }

    if (package_map_find(elm_json->package_dependencies, author, name)) {
        return elm_json->package_dependencies;
    }
    if (package_map_find(elm_json->package_test_dependencies, author, name)) {
        return elm_json->package_test_dependencies;
    }

    return NULL;
}

static void remove_from_all_package_maps(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || elm_json->type != ELM_PROJECT_PACKAGE) {
        return;
    }

    package_map_remove(elm_json->package_dependencies, author, name);
    package_map_remove(elm_json->package_test_dependencies, author, name);
}

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
    printf("Usage: %s install PACKAGE\n", global_context_program_name());
    printf("\n");
    printf("Install packages for your Elm project.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s install elm/html              # Add elm/html to your project\n", global_context_program_name());
    printf("  %s install --test elm/json       # Add elm/json as a test dependency\n", global_context_program_name());
    printf("  %s install --major elm/html      # Upgrade elm/html to next major version\n", global_context_program_name());
    printf("  %s install --from-file ./pkg.zip elm/html  # Install from local file\n", global_context_program_name());
    printf("  %s install --from-url URL elm/html         # Install from URL\n", global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  --test                             # Install as test dependency\n");
    printf("  --upgrade-all                      # Allow upgrading production deps (with --test)\n");
    printf("  --major PACKAGE                    # Allow major version upgrade for package\n");
    printf("  --from-file PATH PACKAGE           # Install from local file/directory\n");
    printf("  --from-url URL PACKAGE             # Install from URL (skips V1 installer SHA check)\n");
    printf("  --local-dev [--from-path PATH] [PACKAGE]\n");
    printf("                                     # Install package for local development\n");
    // printf("  --pin                              # Create PIN file with package version\n");
    printf("  -v, --verbose                      # Show progress reports (registry, connectivity)\n");
    printf("  -q, --quiet                        # Suppress progress reports\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
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

static int install_package(const char *package, bool is_test, bool major_upgrade, bool upgrade_all, bool auto_yes, ElmJson *elm_json, InstallEnv *env) {
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

        /*
         * Handle --test flag specially: when the user explicitly asks for a test
         * dependency, we need to ensure the package ends up in test-dependencies,
         * not just get promoted within production dependencies.
         */
        if (is_test && elm_json->type == ELM_PROJECT_APPLICATION) {
            /* Check if already in test-dependencies/direct */
            if (package_map_find(elm_json->dependencies_test_direct, author, name)) {
                printf("It is already a direct test dependency!\n");
                arena_free(author);
                arena_free(name);
                return 0;
            }
            
            /* Check if in test-dependencies/indirect - promote within test deps */
            Package *test_indirect = package_map_find(elm_json->dependencies_test_indirect, author, name);
            if (test_indirect) {
                package_map_add(elm_json->dependencies_test_direct, author, name, test_indirect->version);
                package_map_remove(elm_json->dependencies_test_indirect, author, name);
                printf("Promoted %s/%s from test-indirect to test-direct dependencies.\n", author, name);
                if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
                    log_error("Failed to write elm.json");
                    arena_free(author);
                    arena_free(name);
                    return 1;
                }
                arena_free(author);
                arena_free(name);
                return 0;
            }
            
            /* Package is in production deps (direct or indirect) - add to test-direct */
            package_map_add(elm_json->dependencies_test_direct, author, name, existing_pkg->version);
            printf("Added %s/%s to test-direct dependencies (already available as production dependency).\n", author, name);
            if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
                log_error("Failed to write elm.json");
                arena_free(author);
                arena_free(name);
                return 1;
            }
            arena_free(author);
            arena_free(name);
            return 0;
        } else if (is_test && elm_json->type == ELM_PROJECT_PACKAGE) {
            /* For packages: check if already in test-dependencies */
            if (package_map_find(elm_json->package_test_dependencies, author, name)) {
                printf("It is already a test dependency!\n");
                arena_free(author);
                arena_free(name);
                return 0;
            }
            
            /* Package is in main deps - add to test deps */
            package_map_add(elm_json->package_test_dependencies, author, name, existing_pkg->version);
            printf("Added %s/%s to test-dependencies (already available as main dependency).\n", author, name);
            if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
                log_error("Failed to write elm.json");
                arena_free(author);
                arena_free(name);
                return 1;
            }
            arena_free(author);
            arena_free(name);
            return 0;
        }

        /* Standard promotion (not --test) */
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

    size_t available_versions = 0;

    if (!package_exists_in_registry(env, author, name, &available_versions)) {
        log_error("I cannot find package '%s/%s'", author, name);
        log_error("Make sure the package name is correct");
        arena_free(author);
        arena_free(name);
        return 1;
    }

    log_debug("Found package in registry with %zu version(s)", available_versions);

    SolverState *solver = solver_init(env, true);
    if (!solver) {
        log_error("Failed to initialize solver");
        arena_free(author);
        arena_free(name);
        return 1;
    }

    InstallPlan *out_plan = NULL;
    SolverResult result = solver_add_package(solver, elm_json, author, name, is_test, major_upgrade, upgrade_all, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to resolve dependencies");

        switch (result) {
            case SOLVER_NO_SOLUTION:
                log_error("No compatible version found for %s/%s", author, name);
                if (is_test && !upgrade_all) {
                    fprintf(stderr, "\n");
                    fprintf(stderr, "When installing a test dependency, production dependencies are pinned\n");
                    fprintf(stderr, "to their current versions. The package %s/%s may require newer\n", author, name);
                    fprintf(stderr, "versions of packages you already have in your production dependencies.\n");
                    fprintf(stderr, "\n");
                    fprintf(stderr, "To see what versions %s/%s requires, run:\n", author, name);
                    fprintf(stderr, "    %s package info %s/%s\n", global_context_program_name(), author, name);
                    fprintf(stderr, "\n");
                    fprintf(stderr, "If there's a version conflict, you can either:\n");
                    fprintf(stderr, "  1. Use --upgrade-all to allow upgrading production dependencies\n");
                    fprintf(stderr, "  2. Upgrade the conflicting production dependency first\n");
                    fprintf(stderr, "  3. Use a different version or alternative package for testing\n");
                }
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
    int max_width = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        int pkg_len = strlen(change->author) + 1 + strlen(change->name);

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
        bool added = false;
        
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            PackageMap *existing_map = find_existing_app_map(elm_json, change->author, change->name);

            if (existing_map) {
                target_map = existing_map;
            } else if (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0) {
                target_map = is_test ? elm_json->dependencies_test_direct : elm_json->dependencies_direct;
            } else {
                target_map = is_test ? elm_json->dependencies_test_indirect : elm_json->dependencies_indirect;
            }

            /* Ensure stale entries do not linger in other maps */
            remove_from_all_app_maps(elm_json, change->author, change->name);
        } else {
            PackageMap *existing_map = find_existing_package_map(elm_json, change->author, change->name);

            if (existing_map) {
                target_map = existing_map;
            } else {
                target_map = is_test ? elm_json->package_test_dependencies : elm_json->package_dependencies;
            }

            remove_from_all_package_maps(elm_json, change->author, change->name);
        }
        
        if (target_map) {
            added = package_map_add(target_map, change->author, change->name, change->new_version);
        }

        if (!target_map || !added) {
            log_error("Failed to record dependency %s/%s %s in elm.json",
                      change->author,
                      change->name,
                      change->new_version ? change->new_version : "(null)");
            install_plan_free(out_plan);
            arena_free(author);
            arena_free(name);
            return 1;
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
    bool upgrade_all = false;
    bool auto_yes = false;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    bool pin_flag = false;
    bool local_dev = false;
    const char *package_name = NULL;
    const char *major_package_name = NULL;
    const char *from_file_path = NULL;
    const char *from_url = NULL;
    const char *from_path = NULL;

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
        } else if (strcmp(argv[i], "--upgrade-all") == 0) {
            upgrade_all = true;
        } else if (strcmp(argv[i], "--pin") == 0) {
            pin_flag = true;
        } else if (strcmp(argv[i], "--from-file") == 0) {
            if (i + 2 < argc) {
                i++;
                from_file_path = argv[i];
                i++;
                package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --from-file requires PATH and PACKAGE arguments\n");
                print_install_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "--from-url") == 0) {
            if (i + 2 < argc) {
                i++;
                from_url = argv[i];
                i++;
                package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --from-url requires URL and PACKAGE arguments\n");
                print_install_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                major_package_name = argv[i];
            } else {
                fprintf(stderr, "Error: --major requires a package name\n");
                print_install_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "--local-dev") == 0) {
            local_dev = true;
        } else if (strcmp(argv[i], "--from-path") == 0) {
            if (i + 1 < argc) {
                i++;
                from_path = argv[i];
            } else {
                fprintf(stderr, "Error: --from-path requires a PATH argument\n");
                print_install_usage();
                return 1;
            }
        } else if (argv[i][0] != '-') {
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

    if (from_file_path && from_url) {
        fprintf(stderr, "Error: Cannot use both --from-file and --from-url\n");
        return 1;
    }

    if (local_dev && (from_file_path || from_url)) {
        fprintf(stderr, "Error: Cannot use --local-dev with --from-file or --from-url\n");
        return 1;
    }

    if (from_path && !local_dev) {
        fprintf(stderr, "Error: --from-path requires --local-dev flag\n");
        return 1;
    }

    if (upgrade_all && !is_test) {
        fprintf(stderr, "Error: --upgrade-all can only be used with --test\n");
        print_install_usage();
        return 1;
    }

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
        log_error("Have you run 'elm init' or '%s init'?", global_context_program_name());
        install_env_free(env);
        return 1;
    }

    int result = 0;

    if (local_dev) {
        /* Handle --local-dev installation */
        const char *source_path = from_path ? from_path : ".";
        
        /*
         * Check if we're running from within a package directory itself.
         * In that case, we just register the package in the cache/registry
         * without trying to add it as a dependency to anything.
         *
         * This is detected when:
         * 1. No explicit --from-path was specified (source defaults to ".")
         * 2. The current elm.json is a package (not an application)
         */
        if (!from_path && elm_json->type == ELM_PROJECT_PACKAGE) {
            /* Register-only mode: just put in cache and registry */
            result = register_local_dev_package(source_path, package_name, env, auto_yes);
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return result;
        }
        
        result = install_local_dev(source_path, package_name, ELM_JSON_PATH, env, is_test, auto_yes);
        
        elm_json_free(elm_json);
        install_env_free(env);
        log_set_level(original_level);
        return result;
    } else if (from_file_path || from_url) {
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

        if (from_url) {
            snprintf(temp_dir_buf, sizeof(temp_dir_buf), "/tmp/wrap_temp_%s_%s", author, name);
            mkdir(temp_dir_buf, DIR_PERMISSIONS);

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

        printf("Successfully installed %s/%s %s!\n", author, name, version);

        if (version) arena_free(version);
        arena_free(author);
        arena_free(name);
        result = 0;
    } else if (package_name) {
        result = install_package(package_name, is_test, major_upgrade, upgrade_all, auto_yes, elm_json, env);
        
        /* If we're in a package directory that's being tracked for local-dev,
         * refresh all dependent applications' indirect dependencies */
        if (result == 0) {
            int refresh_result = refresh_local_dev_dependents(env);
            if (refresh_result != 0) {
                log_error("Warning: Some dependent applications may need manual update");
            }
        }
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
