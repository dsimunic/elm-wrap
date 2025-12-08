#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../solver.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void print_remove_usage(void) {
    printf("Usage: %s package remove <PACKAGE>\n", global_context_program_name());
    printf("\n");
    printf("Remove a package from your Elm project.\n");
    printf("\n");
    printf("This will also remove any indirect dependencies that are no longer\n");
    printf("needed by other packages.\n");
    printf("\n");
    printf("Alias: 'package uninstall' can be used instead of 'package remove'.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package remove elm/html      # Remove elm/html from your project\n", global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
}

/**
 * Find orphaned indirect dependencies after removing a package.
 * Uses the shared find_orphaned_packages function to detect orphans.
 */
static bool find_orphaned_dependencies(
    const ElmJson *elm_json,
    const char *target_author,
    const char *target_name,
    CacheConfig *cache,
    InstallPlan *plan
) {
    if (elm_json->type != ELM_PROJECT_APPLICATION) {
        /* For packages, we don't have the direct/indirect distinction */
        return true;
    }

    log_debug("Finding orphaned dependencies after removing %s/%s", target_author, target_name);

    /* Use the shared orphan detection function */
    PackageMap *orphaned = NULL;
    if (!find_orphaned_packages(elm_json, cache, target_author, target_name, &orphaned)) {
        return false;
    }

    /* Add orphaned packages to the removal plan */
    if (orphaned) {
        for (int i = 0; i < orphaned->count; i++) {
            Package *pkg = &orphaned->packages[i];
            install_plan_add_change(plan, pkg->author, pkg->name, pkg->version, NULL);
        }
        package_map_free(orphaned);
    }

    return true;
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

    SolverState *solver = solver_init(env, false);
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

        if (result == SOLVER_INVALID_PACKAGE) {
            log_error("Package %s/%s is not in your elm.json", author, name);
        }

        elm_json_free(elm_json);
        install_env_free(env);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    /* Find and add orphaned indirect dependencies to the removal plan */
    if (!find_orphaned_dependencies(elm_json, author, name, env->cache, out_plan)) {
        log_error("Failed to find orphaned dependencies");
        install_plan_free(out_plan);
        elm_json_free(elm_json);
        install_env_free(env);
        arena_free(author);
        arena_free(name);
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
    arena_free(author);
    arena_free(name);
    return 0;
}
