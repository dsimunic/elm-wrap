#include "package_common.h"
#include "install_local_dev.h"
#include "../../package_suggestions.h"
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
#include "../../dyn_array.h"
#include "../../log.h"
#include "../../fileutil.h"
#include "../../terminal_colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANSI_DULL_CYAN "\033[36m"
#define ANSI_DULL_YELLOW "\033[33m"
#define ANSI_RESET "\033[0m"
#define AVAILABLE_VERSION_DISPLAY_LIMIT 10

static bool version_exists_in_registry_env(InstallEnv *env, const char *author, const char *name, const Version *target) {
    if (!env || !author || !name || !target) {
        return false;
    }

    if (env->protocol_mode == PROTOCOL_V2) {
        if (!env->v2_registry) {
            return false;
        }
        V2PackageEntry *entry = v2_registry_find(env->v2_registry, author, name);
        if (!entry) {
            return false;
        }
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *ver = &entry->versions[i];
            if (ver->status != V2_STATUS_VALID) {
                continue;
            }
            if (ver->major == target->major &&
                ver->minor == target->minor &&
                ver->patch == target->patch) {
                return true;
            }
        }
        return false;
    }

    if (!env->registry) {
        return false;
    }

    RegistryEntry *entry = registry_find(env->registry, author, name);
    if (!entry) {
        return false;
    }

    for (size_t i = 0; i < entry->version_count; i++) {
        if (version_compare(&entry->versions[i], target) == 0) {
            return true;
        }
    }

    return false;
}

static void print_available_versions_for_package(InstallEnv *env, const char *author, const char *name, size_t limit) {
    fprintf(stderr, "Available versions:\n");
    if (!env) {
        fprintf(stderr, "  (registry unavailable)\n");
        return;
    }

    size_t printed = 0;
    size_t total = 0;

    if (env->protocol_mode == PROTOCOL_V2) {
        if (!env->v2_registry) {
            fprintf(stderr, "  (registry data unavailable)\n");
            return;
        }

        V2PackageEntry *entry = v2_registry_find(env->v2_registry, author, name);
        if (!entry) {
            fprintf(stderr, "  (package not found in registry)\n");
            return;
        }

        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *ver = &entry->versions[i];
            if (ver->status != V2_STATUS_VALID) {
                continue;
            }
            total++;
            if (printed < limit) {
                char *ver_str = version_format(ver->major, ver->minor, ver->patch);
                fprintf(stderr, "  %s\n", ver_str ? ver_str : "(invalid)");
                if (ver_str) {
                    arena_free(ver_str);
                }
                printed++;
            }
        }

        if (total == 0) {
            fprintf(stderr, "  (no published versions)\n");
            return;
        }

        if (total > printed) {
            fprintf(stderr, "  ... and %zu more\n", total - printed);
        }
        return;
    }

    if (!env->registry) {
        fprintf(stderr, "  (registry data unavailable)\n");
        return;
    }

    RegistryEntry *entry = registry_find(env->registry, author, name);
    if (!entry || entry->version_count == 0) {
        fprintf(stderr, "  (no published versions)\n");
        return;
    }

    size_t show_count = entry->version_count < limit ? entry->version_count : limit;
    for (size_t i = 0; i < show_count; i++) {
        char *ver_str = version_to_string(&entry->versions[i]);
        fprintf(stderr, "  %s\n", ver_str ? ver_str : "(invalid)");
        if (ver_str) {
            arena_free(ver_str);
        }
    }

    if (entry->version_count > limit) {
        fprintf(stderr, "  ... and %zu more\n", entry->version_count - limit);
    }
}

static void print_package_suggestions_block(const char *author, const char *name, const PackageSuggestion *suggestions, size_t count) {
    if (!suggestions || count == 0) {
        return;
    }

    fprintf(stderr, "\n%s-- UNKNOWN PACKAGE -------------------------------------------------------------%s\n\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "I could not find '%s/%s' in the package registry, but I found\n", author, name);
    fprintf(stderr, "these packages with similar names:\n\n");

    for (size_t i = 0; i < count && i < MAX_PACKAGE_SUGGESTIONS; i++) {
        fprintf(stderr, "    %s/%s\n", suggestions[i].author, suggestions[i].name);
    }

    fprintf(stderr, "\nMaybe you want one of these instead?\n\n");
}

static void print_target_version_conflict(const char *author, const char *name, const Version *version, bool include_guidance) {
    if (!author || !name || !version) {
        return;
    }

    char *ver_str = version_to_string(version);
    log_error("Cannot install %s/%s at version %s",
              author,
              name,
              ver_str ? ver_str : "(invalid)");
    if (ver_str) {
        arena_free(ver_str);
    }
    if (include_guidance) {
        fprintf(stderr, "\nThe requested version has dependencies that conflict with your\n");
        fprintf(stderr, "current elm.json. You may need to:\n\n");
        fprintf(stderr, "  1. Try a different version\n");
        fprintf(stderr, "  2. Upgrade conflicting packages first\n");
        fprintf(stderr, "  3. Use --major to allow major version upgrades of dependencies\n");
    }
}

/* Note: Helper functions find_package_map() and remove_from_all_app_maps() 
 * are now in package_common.c/h */

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
    printf("Usage: %s install PACKAGE[@VERSION] [PACKAGE[@VERSION]...]\n", global_context_program_name());
    printf("\n");
    printf("Install packages for your Elm project.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s install elm/html                     # Add elm/html to your project\n", global_context_program_name());
    printf("  %s install elm/html@1.0.0               # Add elm/html at specific version\n", global_context_program_name());
    printf("  %s install elm/html 1.0.0               # Same (single package only)\n", global_context_program_name());
    printf("  %s install elm/html elm/json elm/url    # Add multiple packages at once\n", global_context_program_name());
    printf("  %s install elm/html@1.0.0 elm/json      # Mix versioned and latest\n", global_context_program_name());
    printf("  %s install --test elm/json              # Add elm/json as a test dependency\n", global_context_program_name());
    printf("  %s install --major elm/html             # Upgrade elm/html to next major version\n", global_context_program_name());
    printf("  %s install --from-file ./pkg.zip elm/html  # Install from local file\n", global_context_program_name());
    printf("  %s install --from-url URL elm/html         # Install from URL\n", global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  --test                             # Install as test dependency\n");
    printf("  --upgrade-all                      # Allow upgrading production deps (with --test)\n");
    printf("  --major PACKAGE                    # Allow major version upgrade for package (single package only)\n");
    printf("  --from-file PATH PACKAGE           # Install from local file/directory (single package only)\n");
    printf("  --from-url URL PACKAGE             # Install from URL (single package only)\n");
    printf("  --local-dev [--from-path PATH] [PACKAGE]\n");
    printf("                                     # Install package for local development\n");
    printf("  --remove-local-dev                 # Remove package from local-dev tracking\n");
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

static int install_package(const PackageInstallSpec *spec, bool is_test, bool major_upgrade, bool upgrade_all, bool auto_yes, ElmJson *elm_json, InstallEnv *env, const char *elm_json_path) {
    const char *author = spec->author;
    const char *name = spec->name;
    const Version *target_version = spec->has_version ? &spec->version : NULL;

    log_debug("Installing %s/%s%s%s", author, name,
              is_test ? " (test dependency)" : "",
              major_upgrade ? " (major upgrade allowed)" : "");

    Package *existing_pkg = find_existing_package(elm_json, author, name);
    PromotionType promotion = elm_json_find_package(elm_json, author, name);

    if (existing_pkg) {
        if (target_version) {
            Version existing_ver = {0};
            bool parsed_existing = false;
            if (existing_pkg->version) {
                parsed_existing = version_parse_safe(existing_pkg->version, &existing_ver);
            }

            if (parsed_existing && version_equals(&existing_ver, target_version)) {
                printf("%s/%s is already installed at version %s\n",
                       author, name, existing_pkg->version);
                return 0;
            }

            char *ver_str = version_to_string(target_version);
            log_debug("Changing %s/%s from %s to %s",
                      author,
                      name,
                      existing_pkg->version ? existing_pkg->version : "(unknown)",
                      ver_str ? ver_str : "(unknown)");
            if (ver_str) {
                arena_free(ver_str);
            }
            /* Continue to solver to perform the requested version change */
        } else if (!major_upgrade) {
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
                    return 0;
                }
                
                /* Check if in test-dependencies/indirect - promote within test deps */
                Package *test_indirect = package_map_find(elm_json->dependencies_test_indirect, author, name);
                if (test_indirect) {
                    package_map_add(elm_json->dependencies_test_direct, author, name, test_indirect->version);
                    package_map_remove(elm_json->dependencies_test_indirect, author, name);
                    printf("Promoted %s/%s from test-indirect to test-direct dependencies.\n", author, name);
                    if (!elm_json_write(elm_json, elm_json_path)) {
                        log_error("Failed to write elm.json");
                        return 1;
                    }
                    return 0;
                }
                
                /* Package is in production deps (direct or indirect) - add to test-direct */
                package_map_add(elm_json->dependencies_test_direct, author, name, existing_pkg->version);
                printf("Added %s/%s to test-direct dependencies (already available as production dependency).\n", author, name);
                if (!elm_json_write(elm_json, elm_json_path)) {
                    log_error("Failed to write elm.json");
                    return 1;
                }
                return 0;
            } else if (is_test && elm_json->type == ELM_PROJECT_PACKAGE) {
                /* For packages: check if already in test-dependencies */
                if (package_map_find(elm_json->package_test_dependencies, author, name)) {
                    printf("It is already a test dependency!\n");
                    return 0;
                }
                
                /* Package is in main deps - add to test deps */
                package_map_add(elm_json->package_test_dependencies, author, name, existing_pkg->version);
                printf("Added %s/%s to test-dependencies (already available as main dependency).\n", author, name);
                if (!elm_json_write(elm_json, elm_json_path)) {
                    log_error("Failed to write elm.json");
                    return 1;
                }
                return 0;
            }

            /* Standard promotion (not --test) */
            if (promotion != PROMOTION_NONE) {
                if (elm_json_promote_package(elm_json, author, name)) {
                    log_debug("Saving updated elm.json");
                    if (!elm_json_write(elm_json, elm_json_path)) {
                        log_error("Failed to write elm.json");
                        return 1;
                    }
                    log_debug("Done");
                }
            } else {
                printf("It is already installed!\n");
            }

            return 0;
        } else if (major_upgrade) {
            log_debug("Package %s/%s exists at %s, checking for major upgrade", 
                      author, name, existing_pkg->version);
        }
    }

    size_t available_versions = 0;

    if (!package_exists_in_registry(env, author, name, &available_versions)) {
        PackageSuggestion suggestions[MAX_PACKAGE_SUGGESTIONS];
        size_t suggestion_count = package_suggest_nearby_from_env(env, author, name, suggestions);

        if (suggestion_count > 0) {
            log_debug("Package '%s/%s' not found, showing suggestions", author, name);
            print_package_suggestions_block(author, name, suggestions, suggestion_count);
        } else {
            log_error("I cannot find package '%s/%s'", author, name);
            log_error("Make sure the package name is correct");
        }
        return 1;
    }

    log_debug("Found package in registry with %zu version(s)", available_versions);

    if (target_version &&
        !version_exists_in_registry_env(env, author, name, target_version)) {
        char *ver_str = version_to_string(target_version);
        fprintf(stderr, "Error: Version %s not found for package %s/%s\n\n",
                ver_str ? ver_str : "(invalid)",
                author,
                name);
        if (ver_str) {
            arena_free(ver_str);
        }
        print_available_versions_for_package(env, author, name, AVAILABLE_VERSION_DISPLAY_LIMIT);
        return 1;
    }

    SolverState *solver = solver_init(env, install_env_solver_online(env));
    if (!solver) {
        log_error("Failed to initialize solver");
        return 1;
    }

    InstallPlan *out_plan = NULL;
    SolverResult result = solver_add_package(solver, elm_json, author, name, target_version, is_test, major_upgrade, upgrade_all, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to resolve dependencies");

        switch (result) {
            case SOLVER_NO_SOLUTION:
                if (target_version) {
                    print_target_version_conflict(author, name, target_version, true);
                } else {
                    log_error("No compatible version found for %s/%s", author, name);
                }
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
                log_offline_cache_error(env);
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

    int add_count = 0;
    int change_count = 0;
    int max_width = 0;

    /*
     * For packages (type=package), the plan display is simpler:
     * - Only show the requested package being added
     * - Show version as constraint (X.Y.Z <= v < (X+1).0.0)
     * - Don't show "changes" - existing dependencies aren't modified
     *
     * For applications, show all adds and changes with pinned versions.
     */
    bool is_package = (elm_json->type == ELM_PROJECT_PACKAGE);

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];

        /* For packages, only count the requested package */
        if (is_package) {
            if (strcmp(change->author, author) != 0 || strcmp(change->name, name) != 0) {
                continue;
            }
            /* For packages, an existing dependency is not a "change" - we preserve it */
            if (find_package_map(elm_json, change->author, change->name)) {
                continue;
            }
        }

        int pkg_len = strlen(change->author) + 1 + strlen(change->name);

        if (!change->old_version) {
            add_count++;
        } else if (!is_package) {
            /* Only count as "change" for applications */
            change_count++;
        }

        if (pkg_len > max_width) max_width = pkg_len;
    }

    PackageChange *adds = arena_malloc(sizeof(PackageChange) * (add_count > 0 ? add_count : 1));
    PackageChange *changes = arena_malloc(sizeof(PackageChange) * (change_count > 0 ? change_count : 1));

    int add_idx = 0;
    int change_idx = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];

        /* For packages, only include the requested package */
        if (is_package) {
            if (strcmp(change->author, author) != 0 || strcmp(change->name, name) != 0) {
                continue;
            }
            if (find_package_map(elm_json, change->author, change->name)) {
                continue;
            }
        }

        if (!change->old_version) {
            adds[add_idx++] = *change;
        } else if (!is_package) {
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
            PackageChange *c = &adds[i];
            char pkg_name[256];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", c->author, c->name);

            /* For packages, display as constraint; for applications, as pinned version */
            if (is_package) {
                char *constraint = version_to_constraint(c->new_version);
                printf("    %-*s    %s\n", max_width, pkg_name,
                       constraint ? constraint : c->new_version);
                if (constraint) arena_free(constraint);
            } else {
                printf("    %-*s    %s\n", max_width, pkg_name, c->new_version);
            }
        }
        printf("  \n");
    }

    if (change_count > 0) {
        printf("  Change:\n");
        for (int i = 0; i < change_count; i++) {
            PackageChange *c = &changes[i];
            char pkg_name[256];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", c->author, c->name);
            printf("    %-*s    %s => %s\n", max_width, pkg_name,
                   c->old_version, c->new_version);
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
        bool is_requested_package = (strcmp(change->author, author) == 0 &&
                                     strcmp(change->name, name) == 0);

        if (elm_json->type == ELM_PROJECT_PACKAGE) {
            /*
             * For packages (type=package), we handle dependencies differently:
             * - Only add the REQUESTED package (not transitive dependencies)
             * - Existing dependencies are already constraints; don't modify them
             * - New dependencies should be added as constraints, not pinned versions
             *
             * Packages don't have direct/indirect dependencies in elm.json - they
             * only list their direct dependencies with version constraints. The
             * Elm compiler resolves transitive dependencies at build time.
             */

            /* Skip transitive dependencies - only process the requested package */
            if (!is_requested_package) {
                continue;
            }

            /* If the package already exists, don't modify it (preserve existing constraint) */
            if (find_existing_package(elm_json, change->author, change->name)) {
                log_debug("Package %s/%s already exists in elm.json, skipping", change->author, change->name);
                continue;
            }
        }

        /* For applications: determine if this is a direct or indirect dependency.
         * If package already exists, keep it in its current map (update in place). */
        bool is_direct = is_requested_package;
        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            PackageMap *existing_map = find_package_map(elm_json, change->author, change->name);
            if (existing_map) {
                /* Keep in current map - determine if it's a direct map */
                is_direct = (existing_map == elm_json->dependencies_direct ||
                             existing_map == elm_json->dependencies_test_direct);
            }
        }

        if (!add_or_update_package_in_elm_json(elm_json, change->author, change->name,
                                               change->new_version, is_test, is_direct,
                                               true /* remove_first for apps */)) {
            log_error("Failed to record dependency %s/%s %s in elm.json",
                      change->author,
                      change->name,
                      change->new_version ? change->new_version : "(null)");
            install_plan_free(out_plan);
            return 1;
        }
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, elm_json_path)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        return 1;
    }

    /* Register local-dev tracking for all installed packages (if they are local-dev) */
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];
            if (change->new_version) {
                register_local_dev_tracking_if_needed(change->author, change->name,
                                                      change->new_version, elm_json_path);
            }
        }
    }

    printf("Successfully installed %s/%s!\n", author, name);

    install_plan_free(out_plan);
    return 0;
}

/**
 * Print validation errors for multi-package install.
 * Only shows packages that failed validation.
 */
static void print_validation_errors(MultiPackageValidation *validation) {
    fprintf(stderr, "%s-- PACKAGE VALIDATION FAILED --------------------------------------------------%s\n\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "I cannot install these requested packages:\n\n");

    for (int i = 0; i < validation->count; i++) {
        PackageValidationResult *r = &validation->results[i];
        if (r->exists && r->valid_name) {
            /* Skip valid packages - only show failures */
            continue;
        } else if (!r->valid_name) {
            fprintf(stderr, "  %s✗%s %s - %s\n", ANSI_RED, ANSI_RESET, r->author, r->error_msg);
        } else {
            fprintf(stderr, "  %s✗%s %s/%s - %s\n", ANSI_RED, ANSI_RESET, r->author, r->name, r->error_msg);
        }
    }

    fprintf(stderr, "\nPlease fix the specification and try again.\n\n");
    fprintf(stderr, "I didn't install anything yet, as I can only install all specified packages or none.\n");
}

/**
 * Install multiple packages in a single operation.
 *
 * All packages are validated upfront, and any validation errors are
 * reported together before any solving begins. This is an atomic
 * operation: either all packages are installed or none are.
 */

/* Track packages that need promotion from indirect to direct */
typedef struct {
    const char *author;
    const char *name;
    const char *version;
} PromotionInfo;

static int install_multiple_packages(
    const PackageInstallSpec *specs,
    int specs_count,
    bool is_test,
    bool upgrade_all,
    bool auto_yes,
    ElmJson *elm_json,
    InstallEnv *env,
    const char *elm_json_path
) {
    for (int i = 0; i < specs_count; i++) {
        const PackageInstallSpec *spec = &specs[i];
        if (spec->has_version &&
            !version_exists_in_registry_env(env, spec->author, spec->name, &spec->version)) {
            char *ver_str = version_to_string(&spec->version);
            fprintf(stderr, "Error: Version %s not found for package %s/%s\n\n",
                    ver_str ? ver_str : "(invalid)",
                    spec->author,
                    spec->name);
            if (ver_str) {
                arena_free(ver_str);
            }
            print_available_versions_for_package(env,
                                                 spec->author,
                                                 spec->name,
                                                 AVAILABLE_VERSION_DISPLAY_LIMIT);
            return 1;
        }
    }

    log_debug("Installing %d packages%s", specs_count, is_test ? " (test dependencies)" : "");

    /*
     * First pass: identify packages that are already installed as indirect
     * dependencies and need to be promoted to direct. These won't appear
     * in the solver's plan since they're already "solved".
     */
    int promotions_capacity = INITIAL_SMALL_CAPACITY;
    int promotions_count = 0;
    PromotionInfo *promotions = arena_malloc(promotions_capacity * sizeof(PromotionInfo));

    /* Track which packages need solving (not already in direct) */
    int to_solve_capacity = INITIAL_SMALL_CAPACITY;
    int to_solve_count = 0;
    PackageVersionSpec *to_solve = arena_malloc(to_solve_capacity * sizeof(PackageVersionSpec));

    for (int i = 0; i < specs_count; i++) {
        const char *author = specs[i].author;
        const char *name = specs[i].name;

        PromotionType promo = elm_json_find_package(elm_json, author, name);

        if (promo == PROMOTION_INDIRECT_TO_DIRECT) {
            /* Package is in indirect dependencies - need to promote */
            Package *existing = find_existing_package(elm_json, author, name);
            DYNARRAY_PUSH(promotions, promotions_count, promotions_capacity,
                ((PromotionInfo){ .author = arena_strdup(author),
                                  .name = arena_strdup(name),
                                  .version = existing ? arena_strdup(existing->version) : NULL }),
                PromotionInfo);
            log_debug("Package %s/%s will be promoted from indirect to direct", author, name);
        } else if (promo == PROMOTION_NONE) {
            /* Package not installed at all - needs solving */
            /* Pass version spec to solver */
            PackageVersionSpec spec;
            spec.author = author;
            spec.name = name;
            spec.version = specs[i].has_version ? &specs[i].version : NULL;
            DYNARRAY_PUSH(to_solve, to_solve_count, to_solve_capacity, spec, PackageVersionSpec);
        } else {
            /* Already a direct dependency - skip */
            log_debug("Package %s/%s is already a direct dependency", author, name);
        }
    }

    /* If all packages are either already direct or just need promotion, skip solver */
    InstallPlan *out_plan = NULL;
    MultiPackageValidation *validation = NULL;

    if (to_solve_count > 0) {
        SolverState *solver = solver_init(env, install_env_solver_online(env));
        if (!solver) {
            log_error("Failed to initialize solver");
            arena_free(promotions);
            arena_free(to_solve);
            return 1;
        }

        SolverResult result = solver_add_packages(
            solver, elm_json, to_solve, to_solve_count,
            is_test, upgrade_all, &out_plan, &validation
        );

        solver_free(solver);

        if (result != SOLVER_OK) {
            if (validation && validation->invalid_count > 0) {
                print_validation_errors(validation);
                multi_package_validation_free(validation);
                arena_free(promotions);
                arena_free(to_solve);
                return 1;
            }

            log_error("Failed to resolve dependencies");
            switch (result) {
                case SOLVER_NO_SOLUTION:
                    log_error("No compatible solution found for the requested packages");
                    {
                        bool printed_guidance = false;
                        for (int i = 0; i < specs_count; i++) {
                            if (!specs[i].has_version) {
                                continue;
                            }
                            print_target_version_conflict(specs[i].author,
                                                          specs[i].name,
                                                          &specs[i].version,
                                                          !printed_guidance);
                            printed_guidance = true;
                        }
                    }
                    if (is_test && !upgrade_all) {
                        fprintf(stderr, "\n");
                        fprintf(stderr, "When installing test dependencies, production dependencies are pinned\n");
                        fprintf(stderr, "to their current versions. You can use --upgrade-all to allow upgrading\n");
                        fprintf(stderr, "production dependencies if needed.\n");
                    }
                    break;
                case SOLVER_NO_OFFLINE_SOLUTION:
                    log_offline_cache_error(env);
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

            if (validation) multi_package_validation_free(validation);
            arena_free(promotions);
            arena_free(to_solve);
            return 1;
        }
    }

    if (validation) multi_package_validation_free(validation);
    arena_free(to_solve);

    /* Prepare plan display */
    int add_count = 0;
    int change_count = 0;
    int max_width = 0;
    bool is_package = (elm_json->type == ELM_PROJECT_PACKAGE);

    /* Calculate max width for promotions */
    for (int i = 0; i < promotions_count; i++) {
        int pkg_len = strlen(promotions[i].author) + 1 + strlen(promotions[i].name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    /* Count adds and changes from solver plan */
    if (out_plan) {
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];

            /* For packages, only count the requested packages */
            if (is_package) {
                bool is_requested = false;
                for (int j = 0; j < specs_count; j++) {
                    if (strcmp(change->author, specs[j].author) == 0 &&
                        strcmp(change->name, specs[j].name) == 0) {
                        is_requested = true;
                        break;
                    }
                }
                if (!is_requested) continue;
                if (find_package_map(elm_json, change->author, change->name)) continue;
            }

            int pkg_len = strlen(change->author) + 1 + strlen(change->name);
            if (!change->old_version) {
                add_count++;
            } else if (!is_package) {
                change_count++;
            }
            if (pkg_len > max_width) max_width = pkg_len;
        }
    }

    PackageChange *adds = arena_malloc(sizeof(PackageChange) * (add_count > 0 ? add_count : 1));
    PackageChange *changes = arena_malloc(sizeof(PackageChange) * (change_count > 0 ? change_count : 1));

    int add_idx = 0;
    int change_idx = 0;

    if (out_plan) {
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];

            if (is_package) {
                bool is_requested = false;
                for (int j = 0; j < specs_count; j++) {
                    if (strcmp(change->author, specs[j].author) == 0 &&
                        strcmp(change->name, specs[j].name) == 0) {
                        is_requested = true;
                        break;
                    }
                }
                if (!is_requested) continue;
                if (find_package_map(elm_json, change->author, change->name)) continue;
            }

            if (!change->old_version) {
                adds[add_idx++] = *change;
            } else if (!is_package) {
                changes[change_idx++] = *change;
            }
        }
    }

    qsort(adds, add_count, sizeof(PackageChange), compare_package_changes);
    qsort(changes, change_count, sizeof(PackageChange), compare_package_changes);

    /* Display the plan */
    bool has_changes = (add_count > 0 || change_count > 0 || promotions_count > 0);
    
    if (!has_changes) {
        /* All packages are already direct dependencies */
        printf("All requested packages are already direct dependencies!\n");
        if (out_plan) install_plan_free(out_plan);
        arena_free(promotions);
        arena_free(adds);
        arena_free(changes);
        return 0;
    }

    printf("Here is my plan:\n");
    printf("  \n");

    if (add_count > 0) {
        printf("  Add:\n");
        for (int i = 0; i < add_count; i++) {
            PackageChange *c = &adds[i];
            char pkg_name[MAX_PACKAGE_NAME_LENGTH];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", c->author, c->name);

            if (is_package) {
                char *constraint = version_to_constraint(c->new_version);
                printf("    %-*s    %s\n", max_width, pkg_name,
                       constraint ? constraint : c->new_version);
                if (constraint) arena_free(constraint);
            } else {
                printf("    %-*s    %s\n", max_width, pkg_name, c->new_version);
            }
        }
        printf("  \n");
    }

    if (promotions_count > 0) {
        printf("  Promote to direct dependency:\n");
        for (int i = 0; i < promotions_count; i++) {
            char pkg_name[MAX_PACKAGE_NAME_LENGTH];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", promotions[i].author, promotions[i].name);
            printf("    %-*s    %s\n", max_width, pkg_name, 
                   promotions[i].version ? promotions[i].version : "");
        }
        printf("  \n");
    }

    if (change_count > 0) {
        printf("  Change:\n");
        for (int i = 0; i < change_count; i++) {
            PackageChange *c = &changes[i];
            char pkg_name[MAX_PACKAGE_NAME_LENGTH];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", c->author, c->name);
            printf("    %-*s    %s => %s\n", max_width, pkg_name,
                   c->old_version, c->new_version);
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
            if (out_plan) install_plan_free(out_plan);
            arena_free(promotions);
            return 1;
        }

        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            if (out_plan) install_plan_free(out_plan);
            arena_free(promotions);
            return 0;
        }
    }

    /* Apply promotions first */
    for (int i = 0; i < promotions_count; i++) {
        if (!elm_json_promote_package(elm_json, promotions[i].author, promotions[i].name)) {
            log_error("Failed to promote %s/%s", promotions[i].author, promotions[i].name);
            if (out_plan) install_plan_free(out_plan);
            arena_free(promotions);
            return 1;
        }
    }

    /* Apply changes from solver plan */
    if (out_plan) {
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];

            /* Check if this is one of the requested packages */
            bool is_requested_package = false;
            for (int j = 0; j < specs_count; j++) {
                if (strcmp(change->author, specs[j].author) == 0 &&
                    strcmp(change->name, specs[j].name) == 0) {
                    is_requested_package = true;
                    break;
                }
            }

            if (elm_json->type == ELM_PROJECT_PACKAGE) {
                if (!is_requested_package) continue;
                if (find_existing_package(elm_json, change->author, change->name)) {
                    log_debug("Package %s/%s already exists in elm.json, skipping", change->author, change->name);
                    continue;
                }
            }

            bool is_direct = is_requested_package;
            if (elm_json->type == ELM_PROJECT_APPLICATION) {
                PackageMap *existing_map = find_package_map(elm_json, change->author, change->name);
                if (existing_map) {
                    is_direct = (existing_map == elm_json->dependencies_direct ||
                                 existing_map == elm_json->dependencies_test_direct);
                }
            }

            if (!add_or_update_package_in_elm_json(elm_json, change->author, change->name,
                                                   change->new_version, is_test, is_direct, true)) {
                log_error("Failed to record dependency %s/%s %s in elm.json",
                          change->author, change->name,
                          change->new_version ? change->new_version : "(null)");
                install_plan_free(out_plan);
                arena_free(promotions);
                return 1;
            }
        }
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, elm_json_path)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        if (out_plan) install_plan_free(out_plan);
        arena_free(promotions);
        return 1;
    }

    /* Register local-dev tracking for all installed packages */
    if (elm_json->type == ELM_PROJECT_APPLICATION && out_plan) {
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];
            if (change->new_version) {
                register_local_dev_tracking_if_needed(change->author, change->name,
                                                      change->new_version, elm_json_path);
            }
        }
    }

    /* Print success message */
    if (specs_count == 1) {
        printf("Successfully installed %s/%s!\n", specs[0].author, specs[0].name);
    } else {
        printf("Successfully installed %d packages!\n", specs_count);
    }

    if (out_plan) install_plan_free(out_plan);
    arena_free(promotions);
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
    bool remove_local_dev = false;
    const char *major_package_name = NULL;
    const char *from_file_path = NULL;
    const char *from_url = NULL;
    const char *from_path = NULL;

    /* Multi-package support: collect package specs into a dynamic array */
    PackageInstallSpec *specs = NULL;
    int specs_count = 0;
    int specs_capacity = 0;

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
                /* Parse package spec (may include @version) */
                char *author = NULL;
                char *name = NULL;
                Version version = {0};
                bool has_version = false;

                if (strchr(argv[i], '@')) {
                    if (!parse_package_with_version(argv[i], &author, &name, &version)) {
                        fprintf(stderr, "Error: Invalid package specification '%s'\n", argv[i]);
                        print_install_usage();
                        return 1;
                    }
                    has_version = true;
                } else {
                    if (!parse_package_name(argv[i], &author, &name)) {
                        fprintf(stderr, "Error: Invalid package name '%s'\n", argv[i]);
                        print_install_usage();
                        return 1;
                    }
                }

                DYNARRAY_PUSH(specs, specs_count, specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = version,
                        .has_version = has_version
                    }),
                    PackageInstallSpec);
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
                /* Parse package spec (may include @version) */
                char *author = NULL;
                char *name = NULL;
                Version version = {0};
                bool has_version = false;

                if (strchr(argv[i], '@')) {
                    if (!parse_package_with_version(argv[i], &author, &name, &version)) {
                        fprintf(stderr, "Error: Invalid package specification '%s'\n", argv[i]);
                        print_install_usage();
                        return 1;
                    }
                    has_version = true;
                } else {
                    if (!parse_package_name(argv[i], &author, &name)) {
                        fprintf(stderr, "Error: Invalid package name '%s'\n", argv[i]);
                        print_install_usage();
                        return 1;
                    }
                }

                DYNARRAY_PUSH(specs, specs_count, specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = version,
                        .has_version = has_version
                    }),
                    PackageInstallSpec);
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
        } else if (strcmp(argv[i], "--remove-local-dev") == 0) {
            remove_local_dev = true;
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
            /* Parse positional argument as package spec or version */
            Version v;
            /* Check if this is a version string for the previous package (backwards compat) */
            if (specs_count > 0 &&
                !specs[specs_count - 1].has_version &&
                version_parse_safe(argv[i], &v)) {
                /* Previous spec has no version, this looks like a version */
                specs[specs_count - 1].version = v;
                specs[specs_count - 1].has_version = true;
            } else if (strchr(argv[i], '@')) {
                /* Package with embedded version: use parse_package_with_version */
                char *author = NULL;
                char *name = NULL;
                Version ver;
                if (!parse_package_with_version(argv[i], &author, &name, &ver)) {
                    fprintf(stderr, "Error: Invalid package specification '%s'\n", argv[i]);
                    print_install_usage();
                    return 1;
                }
                DYNARRAY_PUSH(specs, specs_count, specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = ver,
                        .has_version = true
                    }),
                    PackageInstallSpec);
            } else {
                /* Package without version (will use latest) */
                char *author = NULL;
                char *name = NULL;
                if (!parse_package_name(argv[i], &author, &name)) {
                    fprintf(stderr, "Error: Invalid package name '%s'\n", argv[i]);
                    print_install_usage();
                    return 1;
                }
                DYNARRAY_PUSH(specs, specs_count, specs_capacity,
                    ((PackageInstallSpec){
                        .author = author,
                        .name = name,
                        .version = {0},
                        .has_version = false
                    }),
                    PackageInstallSpec);
            }
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_install_usage();
            return 1;
        }
    }

    /* Handle --major: requires single package */
    if (major_upgrade) {
        if (!major_package_name) {
            fprintf(stderr, "Error: --major requires a package name\n");
            print_install_usage();
            return 1;
        }
        if (specs_count > 0) {
            fprintf(stderr, "Error: --major can only be used with a single package\n");
            return 1;
        }
        /* Parse major package spec */
        char *author = NULL;
        char *name = NULL;
        Version ver = {0};
        bool has_version = false;

        if (strchr(major_package_name, '@')) {
            if (!parse_package_with_version(major_package_name, &author, &name, &ver)) {
                fprintf(stderr, "Error: Invalid package specification '%s'\n", major_package_name);
                print_install_usage();
                return 1;
            }
            has_version = true;
            /* Warn when both --major and explicit version are specified */
            fprintf(stderr, "Warning: --major flag is ignored when an explicit version is specified\n");
            fprintf(stderr, "         Installing %s/%s at version %u.%u.%u\n",
                    author, name, ver.major, ver.minor, ver.patch);
        } else {
            if (!parse_package_name(major_package_name, &author, &name)) {
                fprintf(stderr, "Error: Invalid package name '%s'\n", major_package_name);
                print_install_usage();
                return 1;
            }
        }

        DYNARRAY_PUSH(specs, specs_count, specs_capacity,
            ((PackageInstallSpec){
                .author = author,
                .name = name,
                .version = ver,
                .has_version = has_version
            }),
            PackageInstallSpec);
    }

    /* Validate flag combinations for single-package-only options */
    if (from_file_path || from_url) {
        if (specs_count > 1) {
            fprintf(stderr, "Error: %s can only install one package at a time\n",
                    from_file_path ? "--from-file" : "--from-url");
            return 1;
        }
    }

    if (from_file_path && from_url) {
        fprintf(stderr, "Error: Cannot use both --from-file and --from-url\n");
        return 1;
    }

    if (local_dev && (from_file_path || from_url)) {
        fprintf(stderr, "Error: Cannot use --local-dev with --from-file or --from-url\n");
        return 1;
    }

    if (remove_local_dev && (from_file_path || from_url || local_dev || from_path)) {
        fprintf(stderr, "Error: --remove-local-dev cannot be combined with other install options\n");
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

    char *project_elm_json_path = find_elm_json_upwards(NULL);
    if (!project_elm_json_path) {
        log_error("Could not find elm.json in current or parent directories");
        log_error("Have you run 'elm init' or '%s init'?", global_context_program_name());
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    log_debug("Reading elm.json (%s)", project_elm_json_path);
    ElmJson *elm_json = elm_json_read(project_elm_json_path);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or '%s init'?", global_context_program_name());
        arena_free(project_elm_json_path);
        install_env_free(env);
        return 1;
    }

    int result = 0;

    if (remove_local_dev) {
        /* Handle --remove-local-dev: unregister package from local-dev tracking */
        elm_json_free(elm_json);
        result = unregister_local_dev_package(env);
        arena_free(project_elm_json_path);
        install_env_free(env);
        log_set_level(original_level);
        return result;
    }

    if (local_dev) {
        /* Handle --local-dev installation */
        const char *source_path = from_path ? from_path : ".";
        /* Reconstruct package name for local-dev (doesn't support versioned specs) */
        const char *package_name = NULL;
        char package_name_buf[MAX_PACKAGE_NAME_LENGTH];
        if (specs_count > 0) {
            snprintf(package_name_buf, sizeof(package_name_buf), "%s/%s",
                     specs[0].author, specs[0].name);
            package_name = package_name_buf;
        }
        
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
            result = register_local_dev_package(source_path, package_name, env, auto_yes, false);
            elm_json_free(elm_json);
            arena_free(project_elm_json_path);
            install_env_free(env);
            log_set_level(original_level);
            return result;
        }
        
        result = install_local_dev(source_path, package_name, project_elm_json_path, env, is_test, auto_yes);
        
        elm_json_free(elm_json);
        arena_free(project_elm_json_path);
        install_env_free(env);
        log_set_level(original_level);
        return result;
    } else if (from_file_path || from_url) {
        if (specs_count == 0) {
            fprintf(stderr, "Error: Package name required for --from-file or --from-url\n");
            elm_json_free(elm_json);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        char *author = specs[0].author;
        char *name = specs[0].name;
        char *version = NULL;
        char *actual_author = NULL;
        char *actual_name = NULL;
        char *actual_version = NULL;
        char temp_dir_buf[1024];
        temp_dir_buf[0] = '\0';

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
                arena_free(project_elm_json_path);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }

            if (!extract_zip_selective(temp_file, temp_dir_buf)) {
                fprintf(stderr, "Error: Failed to extract archive\n");
                arena_free(author);
                arena_free(name);
                elm_json_free(elm_json);
                arena_free(project_elm_json_path);
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
            arena_free(project_elm_json_path);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        char pkg_elm_json_path[2048];
        if (S_ISDIR(st.st_mode)) {
            snprintf(pkg_elm_json_path, sizeof(pkg_elm_json_path), "%s/elm.json", from_file_path);
        } else {
            fprintf(stderr, "Error: --from-file requires a directory path\n");
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            arena_free(project_elm_json_path);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        if (stat(pkg_elm_json_path, &st) != 0) {
            char *found_path = find_package_elm_json(from_file_path);
            if (found_path) {
                snprintf(pkg_elm_json_path, sizeof(pkg_elm_json_path), "%s", found_path);
                arena_free(found_path);
            } else {
                fprintf(stderr, "Error: Could not find elm.json in %s\n", from_file_path);
                arena_free(author);
                arena_free(name);
                elm_json_free(elm_json);
                arena_free(project_elm_json_path);
                install_env_free(env);
                log_set_level(original_level);
                return 1;
            }
        }

        if (read_package_info_from_elm_json(pkg_elm_json_path, &actual_author, &actual_name, &actual_version)) {
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
                        arena_free(project_elm_json_path);
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
            fprintf(stderr, "Error: Could not read package information from %s\n", pkg_elm_json_path);
            arena_free(author);
            arena_free(name);
            elm_json_free(elm_json);
            arena_free(project_elm_json_path);
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
                elm_json_free(elm_json);
                arena_free(project_elm_json_path);
                install_env_free(env);
                log_set_level(original_level);
                return 0;
            }
        }

        if (!install_from_file(from_file_path, env, author, name, version)) {
            fprintf(stderr, "Error: Failed to install package from file\n");
            if (version) arena_free(version);
            elm_json_free(elm_json);
            arena_free(project_elm_json_path);
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

        if (!add_or_update_package_in_elm_json(
                elm_json,
                author,
                name,
                version,
                is_test,
                true,   /* always treat as direct dependency */
                true))  /* remove from other maps before adding */
        {
            fprintf(stderr, "Error: Failed to record %s/%s %s in elm.json\n", author, name, version);
            if (version) arena_free(version);
            elm_json_free(elm_json);
            arena_free(project_elm_json_path);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        printf("Saving elm.json...\n");
        if (!elm_json_write(elm_json, project_elm_json_path)) {
            fprintf(stderr, "Error: Failed to write elm.json\n");
            if (version) arena_free(version);
            elm_json_free(elm_json);
            arena_free(project_elm_json_path);
            install_env_free(env);
            log_set_level(original_level);
            return 1;
        }

        printf("Successfully installed %s/%s %s!\n", author, name, version);

        if (version) arena_free(version);
        result = 0;
    } else if (specs_count > 0) {
        /* Install one or more packages */
        if (specs_count == 1 && major_upgrade) {
            /* Single package with --major flag: use existing install_package */
            result = install_package(&specs[0], is_test, major_upgrade, upgrade_all, auto_yes, elm_json, env, project_elm_json_path);
        } else if (specs_count == 1) {
            /* Single package without special flags: use existing install_package */
            result = install_package(&specs[0], is_test, false, upgrade_all, auto_yes, elm_json, env, project_elm_json_path);
        } else {
            /* Multiple packages: use new multi-package install */
            result = install_multiple_packages(specs, specs_count, is_test, upgrade_all, auto_yes, elm_json, env, project_elm_json_path);
        }
        
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
        arena_free(project_elm_json_path);
        install_env_free(env);
        log_set_level(original_level);

        print_install_what(elm_home);
        arena_free(elm_home);
        return 1;
    }

    elm_json_free(elm_json);
    arena_free(project_elm_json_path);
    install_env_free(env);

    log_set_level(original_level);

    return result;
}
