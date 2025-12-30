#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../solver.h"
#include "../../alloc.h"
#include "../../dyn_array.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include "../../cache.h"
#include "../../terminal_colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char *remove_invocation_to_cmd_path(const char *invocation) {
    if (!invocation) {
        return "package uninstall";
    }

    if (strcmp(invocation, "uninstall") == 0) {
        return "uninstall";
    }

    if (strcmp(invocation, "package remove") == 0) {
        return "package remove";
    }

    if (strcmp(invocation, "package uninstall") == 0) {
        return "package uninstall";
    }

    return "package uninstall";
}

static const char *remove_invocation_alias_line(const char *invocation) {
    if (!invocation) {
        return "Alias: 'package remove' can be used instead of 'package uninstall'.";
    }

    if (strcmp(invocation, "uninstall") == 0) {
        return "Alias: 'package uninstall' can be used instead of 'uninstall'.";
    }

    if (strcmp(invocation, "package remove") == 0) {
        return "Alias: 'package uninstall' can be used instead of 'package remove'.";
    }

    if (strcmp(invocation, "package uninstall") == 0) {
        return "Alias: 'package remove' can be used instead of 'package uninstall'.";
    }

    return "Alias: 'package remove' can be used instead of 'package uninstall'.";
}

static void print_remove_usage(const char *invocation) {
    const char *prog = global_context_program_name();
    const char *cmd_path = remove_invocation_to_cmd_path(invocation);

    printf("Usage:\n");
    printf("  %s %s PACKAGE [PACKAGE...]\n", prog, cmd_path);
    printf("  %s %s --local-dev\n", prog, cmd_path);
    printf("\n");
    printf("Remove packages from your Elm project.\n");
    printf("\n");
    printf("Use --local-dev (run from within an Elm package directory) to remove the\n");
    printf("current package from local-dev tracking.\n");
    printf("\n");
    printf("This will also remove any indirect dependencies that are no longer\n");
    printf("needed by other packages.\n");
    printf("\n");
    printf("%s\n", remove_invocation_alias_line(invocation));
    printf("\n");
    printf("Examples:\n");
    printf("  %s %s elm/html           # Remove elm/html from your project\n", prog, cmd_path);
    printf("  %s %s elm/html elm/json  # Remove multiple packages at once\n", prog, cmd_path);
    printf("  %s %s --local-dev        # Remove current package from local-dev tracking\n", prog, cmd_path);
    printf("\n");
    printf("Options:\n");
    printf("  --local-dev                        # Remove current package from local-dev tracking\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
}

/**
 * Find orphaned indirect dependencies after removing packages.
 * Uses the shared find_orphaned_packages function to detect orphans.
 * For multiple packages, we need to simulate removing all at once.
 */
static bool find_orphaned_dependencies_multi(
    const ElmJson *elm_json,
    const char **authors,
    const char **names,
    int count,
    CacheConfig *cache,
    InstallPlan *plan
) {
    if (elm_json->type != ELM_PROJECT_APPLICATION) {
        /* For packages, we don't have the direct/indirect distinction */
        return true;
    }

    /* For multiple packages, we need to find orphans considering all removals */
    /* Currently find_orphaned_packages only handles single package removal */
    /* We'll call it for each package - orphans may be duplicated but plan merging handles that */
    for (int i = 0; i < count; i++) {
        log_debug("Finding orphaned dependencies after removing %s/%s", authors[i], names[i]);

        PackageMap *orphaned = NULL;
        if (!find_orphaned_packages(elm_json, (struct CacheConfig *)cache, authors[i], names[i], &orphaned)) {
            return false;
        }

        if (orphaned) {
            for (int j = 0; j < orphaned->count; j++) {
                Package *pkg = &orphaned->packages[j];
                /* Check if already in plan to avoid duplicates */
                bool already_in_plan = false;
                for (int k = 0; k < plan->count; k++) {
                    if (strcmp(plan->changes[k].author, pkg->author) == 0 &&
                        strcmp(plan->changes[k].name, pkg->name) == 0) {
                        already_in_plan = true;
                        break;
                    }
                }
                if (!already_in_plan) {
                    install_plan_add_change(plan, pkg->author, pkg->name, pkg->version, NULL);
                }
            }
            package_map_free(orphaned);
        }
    }

    return true;
}

/**
 * Validation result for package removal
 */
typedef struct {
    const char *author;
    const char *name;
    bool valid_name;       /* Name format is valid (author/name) */
    bool exists;           /* Package exists in elm.json */
    const char *error_msg; /* Human-readable error if failed */
} RemoveValidationResult;

typedef struct {
    RemoveValidationResult *results;
    int count;
    int valid_count;
    int invalid_count;
} MultiRemoveValidation;

static void print_remove_validation_errors(MultiRemoveValidation *validation) {
    fprintf(stderr, "%s-- PACKAGE REMOVAL FAILED -----------------------------------------------------%s\n\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "I cannot remove these requested packages:\n\n");

    for (int i = 0; i < validation->count; i++) {
        RemoveValidationResult *r = &validation->results[i];
        if (r->exists && r->valid_name) {
            continue;  /* Skip valid packages */
        } else if (!r->valid_name) {
            fprintf(stderr, "  %s✗%s %s - %s\n", ANSI_RED, ANSI_RESET, r->author, r->error_msg);
        } else {
            fprintf(stderr, "  %s✗%s %s/%s - %s\n", ANSI_RED, ANSI_RESET, r->author, r->name, r->error_msg);
        }
    }

    fprintf(stderr, "\nPlease fix the specification and try again.\n\n");
    fprintf(stderr, "I didn't remove anything yet, as I can only remove all specified packages or none.\n");
}

int cmd_remove(int argc, char *argv[], const char *invocation) {
    /* Multi-package support: collect package names into a dynamic array */
    const char **package_names = NULL;
    int package_count = 0;
    int package_capacity = 0;
    bool auto_yes = false;
    bool remove_local_dev = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_remove_usage(invocation);
            return 0;
        } else if (strcmp(argv[i], "--local-dev") == 0) {
            remove_local_dev = true;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (argv[i][0] != '-') {
            /* Collect package names into array */
            DYNARRAY_PUSH(package_names, package_count, package_capacity, argv[i], const char*);
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_remove_usage(invocation);
            return 1;
        }
    }

    if (remove_local_dev) {
        if (package_count > 0 || auto_yes) {
            fprintf(stderr, "Error: --local-dev cannot be combined with package removal options\n");
            print_remove_usage(invocation);
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

        int result = unregister_local_dev_package(env);
        install_env_free(env);
        return result;
    }

    if (package_count == 0) {
        fprintf(stderr, "Error: At least one package name is required\n");
        print_remove_usage(invocation);
        return 1;
    }

    /* Phase 1: Validate all package names and check they exist in elm.json */
    MultiRemoveValidation validation;
    validation.results = arena_calloc(package_count, sizeof(RemoveValidationResult));
    validation.count = package_count;
    validation.valid_count = 0;
    validation.invalid_count = 0;

    /* We need to read elm.json first to validate package existence */
    log_debug("Reading elm.json");
    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        return 1;
    }

    /* Arrays to hold parsed author/name for valid packages */
    char **authors = arena_malloc(package_count * sizeof(char*));
    char **names = arena_malloc(package_count * sizeof(char*));

    for (int i = 0; i < package_count; i++) {
        RemoveValidationResult *r = &validation.results[i];
        char *author = NULL;
        char *name = NULL;

        /* Parse and validate name format */
        if (!parse_package_name(package_names[i], &author, &name)) {
            r->author = arena_strdup(package_names[i]);
            r->name = NULL;
            r->valid_name = false;
            r->exists = false;
            r->error_msg = "Invalid format (expected author/package)";
            validation.invalid_count++;
            authors[i] = NULL;
            names[i] = NULL;
            continue;
        }

        r->author = author;
        r->name = name;
        r->valid_name = true;
        authors[i] = author;
        names[i] = name;

        /* Check if package exists in elm.json */
        Package *existing = find_existing_package(elm_json, author, name);
        if (!existing) {
            r->exists = false;
            r->error_msg = "Package not in your elm.json";
            validation.invalid_count++;
        } else {
            r->exists = true;
            r->error_msg = NULL;
            validation.valid_count++;
        }
    }

    /* Phase 2: Report validation errors if any */
    if (validation.invalid_count > 0) {
        print_remove_validation_errors(&validation);
        elm_json_free(elm_json);
        return 1;
    }

    /* Log what we're removing */
    if (package_count == 1) {
        log_debug("Removing %s/%s", authors[0], names[0]);
    } else {
        log_debug("Removing %d packages", package_count);
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        elm_json_free(elm_json);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    log_debug("ELM_HOME: %s", env->cache->elm_home);

    /* Phase 3: Build removal plan for all packages */
    InstallPlan *out_plan = install_plan_create();
    if (!out_plan) {
        log_error("Failed to create install plan");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Add all target packages to removal plan */
    for (int i = 0; i < package_count; i++) {
        Package *pkg = find_existing_package(elm_json, authors[i], names[i]);
        if (pkg) {
            install_plan_add_change(out_plan, authors[i], names[i], pkg->version, NULL);
        }
    }

    /* Find and add orphaned indirect dependencies to the removal plan */
    if (!find_orphaned_dependencies_multi(elm_json, (const char **)authors, (const char **)names, 
                                          package_count, env->cache, out_plan)) {
        log_error("Failed to find orphaned dependencies");
        install_plan_free(out_plan);
        elm_json_free(elm_json);
        install_env_free(env);
        return 1;
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
    printf("  Remove:\n");
    
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        char pkg_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
        printf("    %-*s    %s\n", max_width, pkg_name, change->old_version);
    }
    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n] ");
        fflush(stdout);
        
        char response[10];
        if (!fgets(response, sizeof(response), stdin)) {
            fprintf(stderr, "Error reading input\n");
            install_plan_free(out_plan);
            elm_json_free(elm_json);
            install_env_free(env);
            return 1;
        }
        
        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            install_plan_free(out_plan);
            elm_json_free(elm_json);
            install_env_free(env);
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
        return 1;
    }

    /* Print success message */
    if (package_count == 1) {
        printf("Successfully removed %s/%s!\n", authors[0], names[0]);
    } else {
        printf("Successfully removed %d packages!\n", package_count);
    }

    /* If we're in a package directory that's being tracked for local-dev,
     * prune orphaned indirect dependencies from all dependent applications */
    if (elm_json->type == ELM_PROJECT_PACKAGE) {
        int prune_result = prune_local_dev_dependents(env->cache);
        if (prune_result != 0) {
            log_error("Warning: Some dependent applications may need manual update");
        }
    }

    install_plan_free(out_plan);
    elm_json_free(elm_json);
    install_env_free(env);
    return 0;
}
