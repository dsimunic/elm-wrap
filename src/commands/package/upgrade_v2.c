/**
 * upgrade_v2.c - V2 Protocol Package Upgrade Implementation
 *
 * Functions for package upgrade using the V2 protocol.
 * These functions use the V2 registry index - all data is in memory,
 * no network access needed for dependency checking.
 */

#include "upgrade_v2.h"
#include "package_common.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../solver.h"
#include "../../protocol_v2/install.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Removed local parse_version - now using version_parse_safe from package_common.h */

/**
 * Format a V2 version to a string.
 * Returns arena-allocated string that caller must free.
 */
static char *v2_version_to_string(V2PackageVersion *v) {
    return version_format(v->major, v->minor, v->patch);
}

int upgrade_single_package_v2(const char *package, ElmJson *elm_json, InstallEnv *env,
                              bool major_upgrade, bool major_ignore_test, bool auto_yes) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package, &author, &name)) {
        return 1;
    }

    log_debug("Upgrading %s/%s%s%s (V2)", author, name,
              major_upgrade ? " (major allowed)" : "",
              major_ignore_test ? " (ignoring test deps)" : "");

    Package *existing_pkg = find_existing_package(elm_json, author, name);
    if (!existing_pkg) {
        fprintf(stderr, "Error: Package %s/%s is not installed\n", author, name);
        fprintf(stderr, "Run '%s package check' to see available upgrades\n", global_context_program_name());
        arena_free(author);
        arena_free(name);
        return 1;
    }

    V2PackageEntry *entry = v2_registry_find(env->v2_registry, author, name);
    if (!entry) {
        log_error("I cannot find package '%s/%s' in V2 registry", author, name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    /* Find the latest version (V2 versions are sorted newest first) */
    char *latest_version = NULL;
    if (major_upgrade) {
        /* Find first valid version */
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *v = &entry->versions[i];
            if (v->status == V2_STATUS_VALID) {
                latest_version = v2_version_to_string(v);
                break;
            }
        }
    } else {
        Version current_version;
        if (!version_parse_safe(existing_pkg->version, &current_version)) {
            fprintf(stderr, "Error: Invalid version format: %s\n", existing_pkg->version);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        /* Find first valid version with same major */
        for (size_t i = 0; i < entry->version_count; i++) {
            V2PackageVersion *v = &entry->versions[i];
            if (v->status == V2_STATUS_VALID && v->major == current_version.major) {
                latest_version = v2_version_to_string(v);
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
        arena_free(latest_version);
        arena_free(author);
        arena_free(name);
        return 0;
    }

    if (major_upgrade) {
        Version current_version, new_version;
        if (!version_parse_safe(existing_pkg->version, &current_version)) {
            fprintf(stderr, "Error: Invalid version format: %s\n", existing_pkg->version);
            arena_free(latest_version);
            arena_free(author);
            arena_free(name);
            return 1;
        }
        if (!version_parse_safe(latest_version, &new_version)) {
            fprintf(stderr, "Error: Invalid version format: %s\n", latest_version);
            arena_free(latest_version);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        int current_major = current_version.major;
        int new_major = new_version.major;

        if (new_version.major != current_version.major) {
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

                /* Use V2 dependency check */
                if (v2_package_depends_on(pkg->author, pkg->name, pkg->version,
                                          author, name, env->v2_registry)) {
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

            int total_reverse_deps = reverse_deps->count + reverse_deps_test->count;

            if (total_reverse_deps > 0) {
                printf("\nWarning: The following packages depend on %s/%s %d.x.x:\n", author, name, current_major);

                bool has_blocking_deps = false;
                bool has_test_blocking_deps = false;
                PackageMap *blocking_deps = package_map_create();
                PackageMap *blocking_test_deps = package_map_create();

                for (int i = 0; i < reverse_deps->count; i++) {
                    Package *pkg = &reverse_deps->packages[i];

                    V2PackageEntry *dep_entry = v2_registry_find(env->v2_registry, pkg->author, pkg->name);
                    bool has_upgrade = false;

                    if (dep_entry) {
                        /* Find newest valid version */
                        for (size_t j = 0; j < dep_entry->version_count; j++) {
                            V2PackageVersion *v = &dep_entry->versions[j];
                            if (v->status == V2_STATUS_VALID) {
                                char *newest = v2_version_to_string(v);
                                if (newest && strcmp(newest, pkg->version) != 0) {
                                    has_upgrade = true;
                                }
                                arena_free(newest);
                                break;
                            }
                        }
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

                    V2PackageEntry *dep_entry = v2_registry_find(env->v2_registry, pkg->author, pkg->name);
                    bool has_upgrade = false;

                    if (dep_entry) {
                        for (size_t j = 0; j < dep_entry->version_count; j++) {
                            V2PackageVersion *v = &dep_entry->versions[j];
                            if (v->status == V2_STATUS_VALID) {
                                char *newest = v2_version_to_string(v);
                                if (newest && strcmp(newest, pkg->version) != 0) {
                                    has_upgrade = true;
                                }
                                arena_free(newest);
                                break;
                            }
                        }
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
                    arena_free(latest_version);
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
                    arena_free(latest_version);
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

    SolverState *solver = solver_init(env, install_env_solver_online(env));
    if (!solver) {
        log_error("Failed to initialize solver");
        arena_free(latest_version);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    InstallPlan *out_plan = NULL;
    bool is_test = (package_map_find(elm_json->dependencies_test_direct, author, name) != NULL ||
                    package_map_find(elm_json->dependencies_test_indirect, author, name) != NULL ||
                    package_map_find(elm_json->package_test_dependencies, author, name) != NULL);

    SolverResult result = solver_add_package(solver, elm_json, author, name, NULL, is_test, major_upgrade, false, &out_plan);

    solver_free(solver);

    if (result != SOLVER_OK) {
        log_error("Failed to resolve dependencies");

        switch (result) {
            case SOLVER_NO_SOLUTION:
                log_error("No solution found - the upgrade conflicts with current dependencies");
                report_missing_registry_versions_for_elm_json(env, elm_json);
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

        arena_free(latest_version);
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

    PackageChange *adds = arena_malloc(sizeof(PackageChange) * (size_t)add_count);
    PackageChange *changes = arena_malloc(sizeof(PackageChange) * (size_t)change_count);

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

    qsort(adds, (size_t)add_count, sizeof(PackageChange), compare_package_changes);
    qsort(changes, (size_t)change_count, sizeof(PackageChange), compare_package_changes);

    int max_width = 0;
    for (int i = 0; i < add_count; i++) {
        PackageChange *change = &adds[i];
        int pkg_len = (int)strlen(change->author) + 1 + (int)strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }
    for (int i = 0; i < change_count; i++) {
        PackageChange *change = &changes[i];
        int pkg_len = (int)strlen(change->author) + 1 + (int)strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    printf("Here is my plan:\n");
    printf("  \n");

    if (add_count > 0) {
        printf("  Add:\n");
        for (int i = 0; i < add_count; i++) {
            PackageChange *change = &adds[i];
            char pkg_name[MAX_PACKAGE_NAME_LENGTH];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
            printf("    %-*s    %s\n", max_width, pkg_name, change->new_version);
        }
        printf("  \n");
    }

    if (change_count > 0) {
        printf("  Change:\n");
        for (int i = 0; i < change_count; i++) {
            PackageChange *change = &changes[i];
            char pkg_name[MAX_PACKAGE_NAME_LENGTH];
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

        char response[INITIAL_SMALL_CAPACITY];
        if (!fgets(response, sizeof(response), stdin)) {
            fprintf(stderr, "Error reading input\n");
            install_plan_free(out_plan);
            arena_free(latest_version);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        if (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n') {
            printf("Aborted.\n");
            install_plan_free(out_plan);
            arena_free(latest_version);
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
                if (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0) {
                    target_map = is_test ? elm_json->dependencies_test_direct : elm_json->dependencies_direct;
                } else {
                    target_map = is_test ? elm_json->dependencies_test_indirect : elm_json->dependencies_indirect;
                }
                package_map_add(target_map, change->author, change->name, change->new_version);
            }
        } else {
            if (change->old_version) {
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
                target_map = is_test ? elm_json->package_test_dependencies : elm_json->package_dependencies;
                package_map_add(target_map, change->author, change->name, change->new_version);
            }
        }
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        arena_free(latest_version);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    printf("Successfully upgraded %s/%s!\n", author, name);

    install_plan_free(out_plan);
    arena_free(latest_version);
    arena_free(author);
    arena_free(name);
    return 0;
}

int upgrade_all_packages_v2(ElmJson *elm_json, InstallEnv *env,
                            bool major_upgrade, bool major_ignore_test, bool auto_yes) {
    (void)major_ignore_test;

    log_debug("Upgrading all packages%s (V2)", major_upgrade ? " (major allowed)" : "");

    SolverState *solver = solver_init(env, install_env_solver_online(env));
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

    if (out_plan->count == 0) {
        printf("No upgrades available. All packages are at their latest %s version.\n",
               major_upgrade ? "major" : "minor");
        install_plan_free(out_plan);
        return 0;
    }

    qsort(out_plan->changes, (size_t)out_plan->count, sizeof(PackageChange), compare_package_changes);

    int max_width = 0;
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        int pkg_len = (int)strlen(change->author) + 1 + (int)strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    printf("Here is my plan:\n");
    printf("  \n");
    printf("  Change:\n");

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        char pkg_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
        printf("    %-*s    %s => %s\n", max_width, pkg_name,
               change->old_version, change->new_version);
    }
    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n]: ");
        fflush(stdout);

        char response[INITIAL_SMALL_CAPACITY];
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
