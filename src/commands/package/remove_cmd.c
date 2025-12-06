#include "package_common.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../solver.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../rulr/rulr.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/host_helpers.h"
#include "../../rulr/runtime/runtime.h"
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
    printf("Examples:\n");
    printf("  %s package remove elm/html      # Remove elm/html from your project\n", global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
}

/**
 * Recursively insert package_dependency facts for a package and all its transitive dependencies.
 * This builds the complete dependency graph needed for orphan detection.
 */
static void insert_package_dependencies_recursive(
    Rulr *rulr,
    CacheConfig *cache,
    const char *author,
    const char *name,
    const char *version,
    PackageMap *visited
) {
    /* Check if we've already processed this package */
    if (package_map_find(visited, author, name)) {
        return;
    }

    /* Mark as visited */
    package_map_add(visited, author, name, version);

    /* Get the package path in cache */
    char *pkg_path = cache_get_package_path(cache, author, name, version);
    if (!pkg_path) {
        log_debug("Could not get cache path for %s/%s %s", author, name, version);
        return;
    }

    /* Build path to elm.json */
    size_t elm_json_len = strlen(pkg_path) + 12; /* /elm.json\0 */
    char *elm_json_path = arena_malloc(elm_json_len);
    if (!elm_json_path) {
        arena_free(pkg_path);
        return;
    }
    snprintf(elm_json_path, elm_json_len, "%s/elm.json", pkg_path);

    /* Read the package's elm.json */
    ElmJson *pkg_elm_json = elm_json_read(elm_json_path);
    arena_free(elm_json_path);
    arena_free(pkg_path);

    if (!pkg_elm_json) {
        log_debug("Could not read elm.json for %s/%s %s", author, name, version);
        return;
    }

    /* Insert package_dependency facts for this package's dependencies */
    PackageMap *deps = NULL;
    if (pkg_elm_json->type == ELM_PROJECT_PACKAGE) {
        deps = pkg_elm_json->package_dependencies;
    } else {
        /* Applications have both direct and indirect */
        deps = pkg_elm_json->dependencies_direct;
    }

    if (deps) {
        for (int i = 0; i < deps->count; i++) {
            Package *dep = &deps->packages[i];
            rulr_insert_fact_4s(rulr, "package_dependency",
                author, name, dep->author, dep->name);

            /* Recursively process this dependency */
            insert_package_dependencies_recursive(rulr, cache,
                dep->author, dep->name, dep->version, visited);
        }
    }

    /* For applications, also process indirect dependencies */
    if (pkg_elm_json->type == ELM_PROJECT_APPLICATION && pkg_elm_json->dependencies_indirect) {
        for (int i = 0; i < pkg_elm_json->dependencies_indirect->count; i++) {
            Package *dep = &pkg_elm_json->dependencies_indirect->packages[i];
            rulr_insert_fact_4s(rulr, "package_dependency",
                author, name, dep->author, dep->name);

            /* Recursively process this dependency */
            insert_package_dependencies_recursive(rulr, cache,
                dep->author, dep->name, dep->version, visited);
        }
    }

    elm_json_free(pkg_elm_json);
}

/**
 * Find orphaned indirect dependencies using the rulr no_orphaned_packages rule.
 * Returns a list of packages to remove (including the target package).
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

    /* Initialize rulr */
    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        log_error("Failed to initialize rulr: %s", err.message);
        return false;
    }

    /* Load the no_orphaned_packages rule */
    err = rulr_load_rule_file(&rulr, "no_orphaned_packages");
    if (err.is_error) {
        log_error("Failed to load no_orphaned_packages rule: %s", err.message);
        rulr_deinit(&rulr);
        return false;
    }

    /* Insert direct_dependency facts (excluding the target package) */
    if (elm_json->dependencies_direct) {
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            if (strcmp(pkg->author, target_author) == 0 && strcmp(pkg->name, target_name) == 0) {
                continue; /* Skip the target package */
            }
            rulr_insert_fact_2s(&rulr, "direct_dependency", pkg->author, pkg->name);
        }
    }

    if (elm_json->dependencies_test_direct) {
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            if (strcmp(pkg->author, target_author) == 0 && strcmp(pkg->name, target_name) == 0) {
                continue; /* Skip the target package */
            }
            rulr_insert_fact_2s(&rulr, "direct_dependency", pkg->author, pkg->name);
        }
    }

    /* Insert indirect_dependency facts */
    if (elm_json->dependencies_indirect) {
        for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_indirect->packages[i];
            rulr_insert_fact_2s(&rulr, "indirect_dependency", pkg->author, pkg->name);
        }
    }

    if (elm_json->dependencies_test_indirect) {
        for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
            rulr_insert_fact_2s(&rulr, "indirect_dependency", pkg->author, pkg->name);
        }
    }

    /* Build the dependency graph by recursively processing all direct dependencies */
    PackageMap *visited = package_map_create();
    if (!visited) {
        log_error("Failed to create visited package map");
        rulr_deinit(&rulr);
        return false;
    }

    if (elm_json->dependencies_direct) {
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            if (strcmp(pkg->author, target_author) == 0 && strcmp(pkg->name, target_name) == 0) {
                continue; /* Skip the target package */
            }
            insert_package_dependencies_recursive(&rulr, cache,
                pkg->author, pkg->name, pkg->version, visited);
        }
    }

    if (elm_json->dependencies_test_direct) {
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            if (strcmp(pkg->author, target_author) == 0 && strcmp(pkg->name, target_name) == 0) {
                continue; /* Skip the target package */
            }
            insert_package_dependencies_recursive(&rulr, cache,
                pkg->author, pkg->name, pkg->version, visited);
        }
    }

    package_map_free(visited);

    /* Evaluate the rule */
    err = rulr_evaluate(&rulr);
    if (err.is_error) {
        log_error("Failed to evaluate orphaned packages rule: %s", err.message);
        rulr_deinit(&rulr);
        return false;
    }

    /* Get the orphaned packages */
    EngineRelationView orphaned_view = rulr_get_relation(&rulr, "orphaned");
    if (orphaned_view.pred_id >= 0 && orphaned_view.num_tuples > 0) {
        log_debug("Found %d orphaned package(s)", orphaned_view.num_tuples);

        const Tuple *tuples = (const Tuple *)orphaned_view.tuples;
        for (int i = 0; i < orphaned_view.num_tuples; i++) {
            const Tuple *t = &tuples[i];
            if (t->arity != 2 || t->fields[0].kind != VAL_SYM || t->fields[1].kind != VAL_SYM) {
                continue;
            }

            const char *orphan_author = rulr_lookup_symbol(&rulr, t->fields[0].u.sym);
            const char *orphan_name = rulr_lookup_symbol(&rulr, t->fields[1].u.sym);

            if (!orphan_author || !orphan_name) {
                continue;
            }

            log_debug("Orphaned: %s/%s", orphan_author, orphan_name);

            /* Find the version in elm.json */
            Package *pkg = NULL;
            if (elm_json->dependencies_indirect) {
                pkg = package_map_find(elm_json->dependencies_indirect, orphan_author, orphan_name);
            }
            if (!pkg && elm_json->dependencies_test_indirect) {
                pkg = package_map_find(elm_json->dependencies_test_indirect, orphan_author, orphan_name);
            }

            if (pkg) {
                install_plan_add_change(plan, orphan_author, orphan_name, pkg->version, NULL);
            }
        }
    }

    rulr_deinit(&rulr);
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

    install_plan_free(out_plan);
    elm_json_free(elm_json);
    install_env_free(env);
    arena_free(author);
    arena_free(name);
    return 0;
}
