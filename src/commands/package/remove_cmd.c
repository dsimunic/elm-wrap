#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../elm_project.h"
#include "../../install_env.h"
#include "../../solver.h"
#include "../../alloc.h"
#include "../../dyn_array.h"
#include "../../constants.h"
#include "../../protocol_v1/install.h"
#include "../../shared/log.h"
#include "../../cache.h"
#include "../../dependency_usage.h"
#include "../../terminal_colors.h"
#include "../../plural.h"
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

    if (strcmp(invocation, "remove") == 0) {
        return "remove";
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
        return "Aliases: 'remove' and 'package uninstall' can be used instead of 'uninstall'.";
    }

    if (strcmp(invocation, "remove") == 0) {
        return "Aliases: 'uninstall' and 'package remove' can be used instead of 'remove'.";
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
    printf("  %s %s --redundant [PATH] [--yes]\n", prog, cmd_path);
    printf("\n");
    printf("Remove packages from your Elm project.\n");
    printf("\n");
    printf("Use --local-dev (run from within an Elm package directory) to remove the\n");
    printf("current package from local-dev tracking.\n");
    printf("\n");
    printf("Use --redundant in an application to find and remove redundant dependency\n");
    printf("references at PATH (defaults to the current directory). Direct dependencies\n");
    printf("that are still required are demoted to indirect.\n");
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
    printf("  %s %s --redundant --yes  # Remove all redundant dependency references\n", prog, cmd_path);
    printf("  %s %s --redundant ./app  # Remove redundant references from another app\n", prog, cmd_path);
    printf("\n");
    printf("Options:\n");
    printf("  --local-dev                        # Remove current package from local-dev tracking\n");
    printf("  --redundant                        # Remove redundant application dependencies\n");
    printf("  -y, --yes                          # Automatically confirm changes\n");
    printf("  --help                             # Show this help\n");
}

/**
 * Tracking info for each package being removed.
 * Remembers the original map and where to demote (if applicable).
 */
typedef struct {
    const char *author;
    const char *name;
    char *version;          /* arena_strdup'd — safe across map operations */
    PackageMap *source_map; /* original map (direct or test-direct) */
    PackageMap *demote_map; /* indirect counterpart, or NULL if already indirect */
    bool demote;            /* after analysis: true = demote, false = remove */
} RemovalTarget;

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

static void print_unsafe_removal_plan(const PackageMap *missing) {
    fprintf(stderr, "%s-- PACKAGE REMOVAL FAILED -----------------------------------------------------%s\n\n",
            ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "I cannot apply this plan because it would remove dependencies that are\n");
    fprintf(stderr, "still required by packages remaining in elm.json:\n\n");

    for (int i = 0; missing && i < missing->count; i++) {
        Package *pkg = &missing->packages[i];
        fprintf(stderr, "    %s✗%s %s/%s  (needed by %s)\n",
                ANSI_RED, ANSI_RESET, pkg->author, pkg->name, pkg->version);
    }

    fprintf(stderr, "\nI didn't modify elm.json.\n");
}

static bool package_name_is_selected(
    const char **package_names,
    int package_count,
    const char *full_name
) {
    for (int i = 0; i < package_count; i++) {
        if (package_names[i] && strcmp(package_names[i], full_name) == 0) {
            return true;
        }
    }
    return false;
}

static bool append_redundant_package_names(
    const PackageMap *packages,
    const char ***package_names,
    int *package_count,
    int *package_capacity
) {
    if (!packages) return true;

    for (int i = 0; i < packages->count; i++) {
        Package *pkg = &packages->packages[i];
        if (!pkg->author || !pkg->name) return false;

        char full_name[MAX_PACKAGE_NAME_LENGTH];
        int name_len = snprintf(full_name, sizeof(full_name), "%s/%s", pkg->author, pkg->name);
        if (name_len < 0 || name_len >= (int)sizeof(full_name)) return false;
        if (package_name_is_selected(*package_names, *package_count, full_name)) continue;

        char *stored_name = arena_strdup(full_name);
        if (!stored_name) return false;

        if (*package_count >= *package_capacity) {
            int new_capacity = *package_capacity == 0
                ? INITIAL_SMALL_CAPACITY
                : *package_capacity * 2;
            const char **resized = arena_realloc(
                (void *)*package_names, (size_t)new_capacity * sizeof(const char *));
            if (!resized) return false;
            *package_names = resized;
            *package_capacity = new_capacity;
        }

        (*package_names)[(*package_count)++] = stored_name;
    }

    return true;
}

int cmd_remove(int argc, char *argv[], const char *invocation) {
    /* Multi-package support: collect package names into a dynamic array */
    const char **package_names = NULL;
    int package_count = 0;
    int package_capacity = 0;
    bool auto_yes = false;
    bool remove_local_dev = false;
    bool remove_redundant = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_remove_usage(invocation);
            return 0;
        } else if (strcmp(argv[i], "--local-dev") == 0) {
            remove_local_dev = true;
        } else if (strcmp(argv[i], "--redundant") == 0) {
            remove_redundant = true;
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
        if (package_count > 0 || remove_redundant) {
            fprintf(stderr, "Error: --local-dev cannot be combined with other package removal options\n");
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

    const char *redundant_path_arg = NULL;
    if (remove_redundant) {
        if (package_count > 1) {
            fprintf(stderr, "Error: --redundant accepts at most one PATH\n");
            print_remove_usage(invocation);
            return 1;
        }
        if (package_count == 1) {
            redundant_path_arg = package_names[0];
            package_count = 0;
        }
    } else if (package_count == 0) {
        fprintf(stderr, "Error: At least one package name is required\n");
        print_remove_usage(invocation);
        return 1;
    }

    const char *elm_json_path = ELM_JSON_PATH;
    if (redundant_path_arg) {
        char *resolved = elm_resolve_elm_json_path(redundant_path_arg, true);
        if (!resolved) return 1;
        elm_json_path = resolved;
    }

    /* We need to read elm.json first to validate package existence */
    log_debug("Reading elm.json");
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        return 1;
    }

    InstallEnv *env = NULL;
    if (remove_redundant) {
        if (elm_json->type != ELM_PROJECT_APPLICATION) {
            fprintf(stderr, "Error: --redundant requires an application elm.json\n");
            elm_json_free(elm_json);
            return 1;
        }

        env = install_env_create();
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

        PackageMap *unused_direct = NULL;
        PackageMap *orphaned_indirect = NULL;
        bool unused_ok = dependency_usage_find_unused_direct(
            elm_json_path, elm_json, env->cache, &unused_direct);
        bool orphaned_ok = find_orphaned_packages(
            elm_json, env->cache, NULL, NULL, 0, &orphaned_indirect);

        if (!unused_ok || !orphaned_ok) {
            if (unused_direct) package_map_free(unused_direct);
            if (orphaned_indirect) package_map_free(orphaned_indirect);
            log_error("Could not determine redundant dependencies safely");
            install_env_free(env);
            elm_json_free(elm_json);
            return 1;
        }

        dependency_usage_report_redundant(unused_direct, orphaned_indirect);

        bool appended = append_redundant_package_names(
            unused_direct, &package_names, &package_count, &package_capacity) &&
            append_redundant_package_names(
                orphaned_indirect, &package_names, &package_count, &package_capacity);
        if (unused_direct) package_map_free(unused_direct);
        if (orphaned_indirect) package_map_free(orphaned_indirect);

        if (!appended) {
            log_error("Failed to prepare redundant dependency removal");
            install_env_free(env);
            elm_json_free(elm_json);
            return 1;
        }

        if (package_count == 0) {
            printf("No redundant dependencies found.\n");
            install_env_free(env);
            elm_json_free(elm_json);
            return 0;
        }
    }

    /* Phase 1: Validate all package names and check they exist in elm.json */
    MultiRemoveValidation validation;
    validation.results = arena_calloc((size_t)package_count, sizeof(RemoveValidationResult));
    validation.count = package_count;
    validation.valid_count = 0;
    validation.invalid_count = 0;
    if (!validation.results) {
        log_error("Failed to allocate package removal validation");
        if (env) install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Arrays to hold parsed author/name for valid packages */
    char **authors = arena_malloc((size_t)package_count * sizeof(char*));
    char **names = arena_malloc((size_t)package_count * sizeof(char*));
    if (!authors || !names) {
        log_error("Failed to allocate package removal targets");
        if (env) install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

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
        if (env) install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Log what we're removing */
    if (package_count == 1) {
        log_debug("Removing %s/%s", authors[0], names[0]);
    } else {
        log_debug("Removing %d packages", package_count);
    }

    if (!env) {
        env = install_env_create();
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
    }

    log_debug("ELM_HOME: %s", env->cache->elm_home);

    if (env->protocol_mode == PROTOCOL_V1) {
        (void)v1_report_project_package_cycles(elm_json, env);
    }

    /* Phase 3: Build removal plan with demotion support for applications */
    InstallPlan *out_plan = install_plan_create();
    if (!out_plan) {
        log_error("Failed to create install plan");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Number of requested packages demoted to indirect (vs. actually removed).
     * Stays 0 for package-type projects and for pure removals; used both for
     * the plan display and for the final success message. */
    int demote_count = 0;

    /* For package-type projects: no direct/indirect distinction, just remove */
    if (elm_json->type != ELM_PROJECT_APPLICATION) {
        for (int i = 0; i < package_count; i++) {
            Package *pkg = find_existing_package(elm_json, authors[i], names[i]);
            if (pkg) {
                install_plan_add_change(out_plan, authors[i], names[i], pkg->version, NULL);
            }
        }
        goto display_plan;
    }

    /* --- Application project: detect demotions via temporary move + orphan detection --- */
    {
        RemovalTarget *targets = arena_calloc(package_count, sizeof(RemovalTarget));

        /* Step 1: Record target info and determine source/demote maps */
        for (int i = 0; i < package_count; i++) {
            targets[i].author = authors[i];
            targets[i].name = names[i];
            targets[i].demote = false;

            PackageMap *src = find_package_map(elm_json, authors[i], names[i]);
            targets[i].source_map = src;

            if (src == elm_json->dependencies_direct) {
                targets[i].demote_map = elm_json->dependencies_indirect;
            } else if (src == elm_json->dependencies_test_direct) {
                targets[i].demote_map = elm_json->dependencies_test_indirect;
            } else {
                /* Already indirect (or not found) — no demotion possible */
                targets[i].demote_map = NULL;
            }

            /* Save version (arena_strdup'd) before any map operations */
            Package *pkg = find_existing_package(elm_json, authors[i], names[i]);
            targets[i].version = pkg ? arena_strdup(pkg->version) : NULL;
        }

        /* Step 2: Temporarily move direct targets to their indirect counterpart */
        for (int i = 0; i < package_count; i++) {
            if (targets[i].demote_map && targets[i].version) {
                /* add-before-remove: package_map_add copies strings */
                package_map_add(targets[i].demote_map, targets[i].author, targets[i].name, targets[i].version);
                package_map_remove(targets[i].source_map, targets[i].author, targets[i].name);
            }
        }

        /* Step 3: Run orphan detection with NO excludes —
         * targets are now "indirect" so the engine checks their reachability */
        PackageMap *orphaned = NULL;
        bool orphan_ok = find_orphaned_packages(elm_json, env->cache, NULL, NULL, 0, &orphaned);

        /* Step 4: Restore elm_json to original state (move targets back) */
        for (int i = 0; i < package_count; i++) {
            if (targets[i].demote_map && targets[i].version) {
                package_map_add(targets[i].source_map, targets[i].author, targets[i].name, targets[i].version);
                package_map_remove(targets[i].demote_map, targets[i].author, targets[i].name);
            }
        }

        if (!orphan_ok) {
            log_error("Failed to find orphaned dependencies");
            install_plan_free(out_plan);
            elm_json_free(elm_json);
            install_env_free(env);
            return 1;
        }

        /* Step 5: Classify each target as demote or remove */
        for (int i = 0; i < package_count; i++) {
            if (!targets[i].demote_map) {
                /* Already indirect — always remove */
                targets[i].demote = false;
                continue;
            }

            /* Check if this target appeared in the orphaned set */
            bool is_orphaned = orphaned &&
                package_map_find(orphaned, targets[i].author, targets[i].name);
            targets[i].demote = !is_orphaned;
        }

        /* Step 6: Build the plan — removals for orphaned targets */
        for (int i = 0; i < package_count; i++) {
            if (!targets[i].demote && targets[i].version) {
                install_plan_add_change(out_plan, targets[i].author, targets[i].name,
                                        targets[i].version, NULL);
            }
        }

        /* Also add orphaned non-target indirect deps (cascade removals) */
        if (orphaned) {
            for (int j = 0; j < orphaned->count; j++) {
                Package *pkg = &orphaned->packages[j];
                /* Skip if this orphan is one of our targets (already handled above) */
                bool is_target = false;
                for (int i = 0; i < package_count; i++) {
                    if (strcmp(pkg->author, targets[i].author) == 0 &&
                        strcmp(pkg->name, targets[i].name) == 0) {
                        is_target = true;
                        break;
                    }
                }
                if (!is_target) {
                    install_plan_add_change(out_plan, pkg->author, pkg->name, pkg->version, NULL);
                }
            }
            package_map_free(orphaned);
        }

        /* Sort the removal plan */
        if (out_plan->count > 0) {
            qsort(out_plan->changes, out_plan->count, sizeof(PackageChange), compare_package_changes);
        }

        /* --- Display plan --- */
        for (int i = 0; i < package_count; i++) {
            if (targets[i].demote) demote_count++;
        }

        /* Compute max width across all entries for alignment */
        int max_width = 0;
        for (int i = 0; i < package_count; i++) {
            if (targets[i].demote) {
                int pkg_len = (int)(strlen(targets[i].author) + 1 + strlen(targets[i].name));
                if (pkg_len > max_width) max_width = pkg_len;
            }
        }
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];
            int pkg_len = (int)(strlen(change->author) + 1 + strlen(change->name));
            if (pkg_len > max_width) max_width = pkg_len;
        }

        printf("Here is my plan:\n");
        printf("  \n");

        if (demote_count > 0) {
            printf("  Demote to indirect:\n");
            for (int i = 0; i < package_count; i++) {
                if (!targets[i].demote) continue;
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", targets[i].author, targets[i].name);
                printf("    %-*s    %s\n", max_width, pkg_name, targets[i].version);
            }
            if (out_plan->count > 0) printf("  \n");
        }

        if (out_plan->count > 0) {
            printf("  Remove:\n");
            for (int i = 0; i < out_plan->count; i++) {
                PackageChange *change = &out_plan->changes[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", change->author, change->name);
                printf("    %-*s    %s\n", max_width, pkg_name, change->old_version);
            }
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

        /* Apply demotions: move from direct to indirect */
        for (int i = 0; i < package_count; i++) {
            if (targets[i].demote && targets[i].version) {
                /* add-before-remove pattern */
                package_map_add(targets[i].demote_map, targets[i].author, targets[i].name, targets[i].version);
                package_map_remove(targets[i].source_map, targets[i].author, targets[i].name);
            }
        }

        /* Apply removals */
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];
            remove_from_all_app_maps(elm_json, change->author, change->name);
        }

        goto save_and_finish;
    }

display_plan:
    /* Display plan for package-type projects (removals only) */
    qsort(out_plan->changes, out_plan->count, sizeof(PackageChange), compare_package_changes);

    {
        int max_width = 0;
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];
            int pkg_len = (int)(strlen(change->author) + 1 + strlen(change->name));
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
            package_map_remove(elm_json->package_dependencies, change->author, change->name);
            package_map_remove(elm_json->package_test_dependencies, change->author, change->name);
        }
    }

save_and_finish:
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        PackageMap *missing = NULL;
        int missing_count = find_missing_dependencies(elm_json, env->cache, &missing);
        if (missing_count > 0 && missing) {
            fflush(stdout);
            print_unsafe_removal_plan(missing);
            package_map_free(missing);
            install_plan_free(out_plan);
            elm_json_free(elm_json);
            install_env_free(env);
            return 1;
        }
    }

    printf("Saving elm.json...\n");
    if (!elm_json_write(elm_json, elm_json_path)) {
        fprintf(stderr, "Error: Failed to write elm.json\n");
        install_plan_free(out_plan);
        elm_json_free(elm_json);
        install_env_free(env);
        return 1;
    }

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        for (int i = 0; i < out_plan->count; i++) {
            PackageChange *change = &out_plan->changes[i];
            if (!unregister_local_dev_tracking_for_app(
                    change->author, change->name, change->old_version, elm_json_path)) {
                log_error("elm.json was updated, but local-dev tracking cleanup failed for %s/%s",
                          change->author, change->name);
                install_plan_free(out_plan);
                elm_json_free(elm_json);
                install_env_free(env);
                return 1;
            }
        }
    }

    /* Print success message — reflect demotions vs. removals so the wording
     * matches the plan we just carried out. */
    int removed_count = out_plan->count;
    if (package_count == 1 && demote_count == 1 && removed_count == 0) {
        printf("Successfully demoted %s/%s to indirect!\n", authors[0], names[0]);
    } else if (package_count == 1 && demote_count == 0 && removed_count == 1) {
        printf("Successfully removed %s/%s!\n", authors[0], names[0]);
    } else if (demote_count == 0) {
        printf("Successfully removed %d package%s!\n",
               removed_count, en_plural_suffix(removed_count));
    } else if (removed_count == 0) {
        printf("Successfully demoted %d package%s to indirect!\n",
               demote_count, en_plural_suffix(demote_count));
    } else {
        printf("Successfully demoted %d package%s to indirect and removed %d package%s!\n",
               demote_count, en_plural_suffix(demote_count),
               removed_count, en_plural_suffix(removed_count));
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
