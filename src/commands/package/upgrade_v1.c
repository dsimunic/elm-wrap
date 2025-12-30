/**
 * upgrade_v1.c - V1 Protocol Package Upgrade Implementation
 *
 * Functions for package upgrade using the V1 protocol.
 * These functions use the V1 registry format and require network access
 * to download packages for dependency checking.
 */

#include "upgrade_v1.h"
#include "package_common.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../solver.h"
#include "../../protocol_v1/install.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Removed local parse_version - now using version_parse_safe from package_common.h */

typedef struct {
    bool in_deps_direct;
    bool in_deps_indirect;
    bool in_deps_test_direct;
    bool in_deps_test_indirect;
    bool in_pkg_deps;
    bool in_pkg_test_deps;
    int count;
} PackagePresence;

static PackagePresence detect_package_presence(const ElmJson *elm_json, const char *author, const char *name) {
    PackagePresence p;
    memset(&p, 0, sizeof(p));

    if (!elm_json || !author || !name) {
        return p;
    }

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        p.in_deps_direct = (elm_json->dependencies_direct && package_map_find(elm_json->dependencies_direct, author, name));
        p.in_deps_indirect = (elm_json->dependencies_indirect && package_map_find(elm_json->dependencies_indirect, author, name));
        p.in_deps_test_direct = (elm_json->dependencies_test_direct && package_map_find(elm_json->dependencies_test_direct, author, name));
        p.in_deps_test_indirect = (elm_json->dependencies_test_indirect && package_map_find(elm_json->dependencies_test_indirect, author, name));
    } else {
        p.in_pkg_deps = (elm_json->package_dependencies && package_map_find(elm_json->package_dependencies, author, name));
        p.in_pkg_test_deps = (elm_json->package_test_dependencies && package_map_find(elm_json->package_test_dependencies, author, name));
    }

    p.count = (p.in_deps_direct ? 1 : 0) + (p.in_deps_indirect ? 1 : 0) +
              (p.in_deps_test_direct ? 1 : 0) + (p.in_deps_test_indirect ? 1 : 0) +
              (p.in_pkg_deps ? 1 : 0) + (p.in_pkg_test_deps ? 1 : 0);
    return p;
}

static bool set_pkg_version_in_map(ElmJson *elm_json, PackageMap *map, const char *author, const char *name, const char *new_version) {
    if (!elm_json || !map || !author || !name || !new_version) {
        return false;
    }

    Package *pkg = package_map_find(map, author, name);
    if (!pkg) {
        return false;
    }

    const char *version_to_write = new_version;
    char *constraint = NULL;
    if (elm_json->type == ELM_PROJECT_PACKAGE) {
        constraint = version_to_constraint(new_version);
        if (constraint) {
            version_to_write = constraint;
        }
    }

    arena_free(pkg->version);
    pkg->version = arena_strdup(version_to_write);

    if (constraint) {
        arena_free(constraint);
    }

    return pkg->version != NULL;
}

static bool apply_change_preserving_location(
    ElmJson *elm_json,
    const char *author,
    const char *name,
    const char *new_version,
    bool default_is_test,
    bool default_is_direct
) {
    if (!elm_json || !author || !name || !new_version) {
        return false;
    }

    PackagePresence presence = detect_package_presence(elm_json, author, name);

    if (presence.count > 1) {
        /* Malformed elm.json: update all occurrences to keep it consistent. */
        bool ok = true;
        if (presence.in_deps_direct) ok = ok && set_pkg_version_in_map(elm_json, elm_json->dependencies_direct, author, name, new_version);
        if (presence.in_deps_indirect) ok = ok && set_pkg_version_in_map(elm_json, elm_json->dependencies_indirect, author, name, new_version);
        if (presence.in_deps_test_direct) ok = ok && set_pkg_version_in_map(elm_json, elm_json->dependencies_test_direct, author, name, new_version);
        if (presence.in_deps_test_indirect) ok = ok && set_pkg_version_in_map(elm_json, elm_json->dependencies_test_indirect, author, name, new_version);
        if (presence.in_pkg_deps) ok = ok && set_pkg_version_in_map(elm_json, elm_json->package_dependencies, author, name, new_version);
        if (presence.in_pkg_test_deps) ok = ok && set_pkg_version_in_map(elm_json, elm_json->package_test_dependencies, author, name, new_version);
        return ok;
    }

    if (presence.count == 1) {
        /* Preserve where it currently lives. */
        bool is_test = false;
        bool is_direct = true;

        if (elm_json->type == ELM_PROJECT_APPLICATION) {
            PackageMap *map = find_package_map(elm_json, author, name);
            if (!map) {
                return false;
            }

            is_test = (map == elm_json->dependencies_test_direct || map == elm_json->dependencies_test_indirect);
            is_direct = (map == elm_json->dependencies_direct || map == elm_json->dependencies_test_direct);
        } else {
            PackageMap *map = find_package_map(elm_json, author, name);
            if (!map) {
                return false;
            }
            is_test = (map == elm_json->package_test_dependencies);
            is_direct = true;
        }

        return add_or_update_package_in_elm_json(elm_json, author, name, new_version, is_test, is_direct, false);
    }

    /* Not currently present: add it in the default location. */
    return add_or_update_package_in_elm_json(elm_json, author, name, new_version, default_is_test, default_is_direct,
                                             elm_json->type == ELM_PROJECT_APPLICATION);
}

int upgrade_single_package_v1(const char *package, ElmJson *elm_json, InstallEnv *env,
                              bool major_upgrade, bool major_ignore_test, bool auto_yes) {
    char *author = NULL;
    char *name = NULL;

    if (!parse_package_name(package, &author, &name)) {
        return 1;
    }

    log_debug("Upgrading %s/%s%s%s (V1)", author, name,
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

    RegistryEntry *registry_entry = registry_find(env->registry, author, name);
    if (!registry_entry) {
        log_error("I cannot find package '%s/%s' in registry", author, name);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    const char *latest_version = NULL;
    if (major_upgrade) {
        if (registry_entry->version_count > 0) {
            latest_version = version_to_string(&registry_entry->versions[0]);
        }
    } else {
        Version current_version;
        if (!version_parse_safe(existing_pkg->version, &current_version)) {
            fprintf(stderr, "Error: Invalid version format: %s\n", existing_pkg->version);
            arena_free(author);
            arena_free(name);
            return 1;
        }

        for (size_t i = 0; i < registry_entry->version_count; i++) {
            Version *v = &registry_entry->versions[i];
            if (v->major == current_version.major) {
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

    if (major_upgrade) {
        Version current_version, new_version;
        if (!version_parse_safe(existing_pkg->version, &current_version)) {
            fprintf(stderr, "Error: Invalid version format: %s\n", existing_pkg->version);
            arena_free((char*)latest_version);
            arena_free(author);
            arena_free(name);
            return 1;
        }
        if (!version_parse_safe(latest_version, &new_version)) {
            fprintf(stderr, "Error: Invalid version format: %s\n", latest_version);
            arena_free((char*)latest_version);
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

                if (v1_package_depends_on(pkg->author, pkg->name, pkg->version, author, name, env)) {
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

    SolverState *solver = solver_init(env, install_env_solver_online(env));
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

        arena_free((char*)latest_version);
        arena_free(author);
        arena_free(name);
        return 1;
    }

    bool is_package_project = (elm_json->type == ELM_PROJECT_PACKAGE);

    int add_count = 0;
    int change_count = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        bool is_requested_package = (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0);

        /* For package projects, only upgrade the requested direct dependency in elm.json. */
        if (is_package_project && !is_requested_package) {
            continue;
        }

        if (!change->old_version) {
            add_count++;
        } else {
            change_count++;
        }
    }

    PackageChange *adds = arena_malloc(sizeof(PackageChange) * (size_t)(add_count > 0 ? add_count : 1));
    PackageChange *changes = arena_malloc(sizeof(PackageChange) * (size_t)(change_count > 0 ? change_count : 1));

    int add_idx = 0;
    int change_idx = 0;

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        bool is_requested_package = (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0);
        if (is_package_project && !is_requested_package) {
            continue;
        }

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
            const char *new_display = change->new_version;
            char *new_constraint = NULL;
            if (is_package_project && change->new_version) {
                new_constraint = version_to_constraint(change->new_version);
                if (new_constraint) {
                    new_display = new_constraint;
                }
            }
            printf("    %-*s    %s\n", max_width, pkg_name, new_display ? new_display : "(none)");
            if (new_constraint) {
                arena_free(new_constraint);
            }
        }
        printf("  \n");
    }

    if (change_count > 0) {
        printf("  Change:\n");
        for (int i = 0; i < change_count; i++) {
            PackageChange *change = &changes[i];
            char pkg_name[MAX_PACKAGE_NAME_LENGTH];
            snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
            const char *old_display = change->old_version;
            const char *new_display = change->new_version;
            char *old_constraint = NULL;
            char *new_constraint = NULL;

            if (is_package_project) {
                if (change->old_version) {
                    old_constraint = version_to_constraint(change->old_version);
                    if (old_constraint) {
                        old_display = old_constraint;
                    }
                }
                if (change->new_version) {
                    new_constraint = version_to_constraint(change->new_version);
                    if (new_constraint) {
                        new_display = new_constraint;
                    }
                }
            }

            printf("    %-*s    %s => %s\n", max_width, pkg_name,
                   old_display ? old_display : "(none)",
                   new_display ? new_display : "(none)");

            if (old_constraint) arena_free(old_constraint);
            if (new_constraint) arena_free(new_constraint);
        }
    }

    arena_free(adds);
    arena_free(changes);

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n] ");
        fflush(stdout);

        char response[INITIAL_SMALL_CAPACITY];
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
        bool is_requested_package = (strcmp(change->author, author) == 0 && strcmp(change->name, name) == 0);

        if (is_package_project && !is_requested_package) {
            continue;
        }

        bool default_is_direct = is_requested_package;
        bool default_is_test = is_test;

        if (!apply_change_preserving_location(elm_json,
                                              change->author,
                                              change->name,
                                              change->new_version,
                                              default_is_test,
                                              default_is_direct)) {
            log_error("Failed to update elm.json for %s/%s", change->author, change->name);
            install_plan_free(out_plan);
            arena_free((char*)latest_version);
            arena_free(author);
            arena_free(name);
            return 1;
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

int upgrade_all_packages_v1(ElmJson *elm_json, InstallEnv *env,
                            bool major_upgrade, bool major_ignore_test, bool auto_yes) {
    (void)major_ignore_test;

    log_debug("Upgrading all packages%s (V1)", major_upgrade ? " (major allowed)" : "");

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

    bool is_package_project = (elm_json->type == ELM_PROJECT_PACKAGE);

    int display_count = 0;
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];

        if (is_package_project) {
            /* For package projects, only upgrade dependencies explicitly listed in elm.json. */
            if (!find_package_map(elm_json, change->author, change->name)) {
                continue;
            }
        }
        display_count++;
    }

    if (display_count == 0) {
        printf("No upgrades available. All packages are at their latest %s version.\n",
               major_upgrade ? "major" : "minor");
        install_plan_free(out_plan);
        return 0;
    }

    int max_width = 0;
    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        if (is_package_project && !find_package_map(elm_json, change->author, change->name)) {
            continue;
        }

        int pkg_len = (int)strlen(change->author) + 1 + (int)strlen(change->name);
        if (pkg_len > max_width) max_width = pkg_len;
    }

    printf("Here is my plan:\n");
    printf("  \n");
    printf("  Change:\n");

    for (int i = 0; i < out_plan->count; i++) {
        PackageChange *change = &out_plan->changes[i];
        if (is_package_project && !find_package_map(elm_json, change->author, change->name)) {
            continue;
        }

        char pkg_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
        const char *old_display = change->old_version;
        const char *new_display = change->new_version;
        char *old_constraint = NULL;
        char *new_constraint = NULL;

        if (is_package_project) {
            if (change->old_version) {
                old_constraint = version_to_constraint(change->old_version);
                if (old_constraint) {
                    old_display = old_constraint;
                }
            }
            if (change->new_version) {
                new_constraint = version_to_constraint(change->new_version);
                if (new_constraint) {
                    new_display = new_constraint;
                }
            }
        }

        printf("    %-*s    %s => %s\n", max_width, pkg_name,
               old_display ? old_display : "(none)",
               new_display ? new_display : "(none)");

        if (old_constraint) arena_free(old_constraint);
        if (new_constraint) arena_free(new_constraint);
    }
    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to update your elm.json accordingly? [Y/n] ");
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

        if (is_package_project && !find_package_map(elm_json, change->author, change->name)) {
            continue;
        }

        if (!apply_change_preserving_location(elm_json,
                                              change->author,
                                              change->name,
                                              change->new_version,
                                              false,
                                              true)) {
            log_error("Failed to update elm.json for %s/%s", change->author, change->name);
            install_plan_free(out_plan);
            return 1;
        }
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        return 1;
    }

    printf("Successfully upgraded %d package(s)!\n", display_count);

    install_plan_free(out_plan);
    return 0;
}
