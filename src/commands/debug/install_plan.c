#include "debug.h"
#include "../../alloc.h"
#include "../../progname.h"
#include "../../log.h"
#include "../../solver.h"
#include "../../install_env.h"
#include "../../elm_json.h"
#include "../../fileutil.h"
#include "../../global_context.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../protocol_v2/solver/pg_elm_v2.h"
#include "../../pgsolver/pg_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

static char *find_elm_json(void) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) {
        return NULL;
    }

    char *path = arena_strdup(cwd);
    if (!path) {
        return NULL;
    }

    while (true) {
        size_t dir_len = strlen(path);
        size_t buf_len = dir_len + sizeof("/elm.json");
        char *candidate = arena_malloc(buf_len);
        if (!candidate) {
            arena_free(path);
            return NULL;
        }

        snprintf(candidate, buf_len, "%s/elm.json", path);
        if (file_exists(candidate)) {
            arena_free(path);
            return candidate;
        }
        arena_free(candidate);

        char *slash = strrchr(path, '/');
        if (!slash || slash == path) {
            break;
        }
        *slash = '\0';
    }

    arena_free(path);
    return NULL;
}

static void print_install_plan_usage(void) {
    printf("Usage: %s debug install-plan <package> [OPTIONS]\n", program_name);
    printf("\n");
    printf("Show what packages would be installed for a package (dry-run).\n");
    printf("This exercises the dependency solver without actually installing anything.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <package>          Package name in author/name format (e.g., elm/html)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --test             Show plan for test dependency\n");
    printf("  --major            Allow major version upgrades\n");
    printf("  -v, --verbose      Show detailed logging output (default)\n");
    printf("  -vv                Show extra verbose (trace) logging output\n");
    printf("  -q, --quiet        Suppress statistics output\n");
    printf("  -h, --help         Show this help message\n");
}

/* Report obvious conflicts between the target package and current pinned deps (V2 only) */
static void report_conflicts_v2(const InstallEnv *env, ElmJson *elm_json, const char *author, const char *name) {
    if (!env || env->protocol_mode != PROTOCOL_V2 || !env->v2_registry || !elm_json) {
        return;
    }

    V2PackageEntry *entry = v2_registry_find(env->v2_registry, author, name);
    if (!entry || entry->version_count == 0) {
        return;
    }

    /* Use latest valid version */
    V2PackageVersion *version = NULL;
    for (size_t i = 0; i < entry->version_count; i++) {
        if (entry->versions[i].status == V2_STATUS_VALID) {
            version = &entry->versions[i];
            break;
        }
    }
    if (!version) {
        return;
    }

    /* Helper to look up current pinned version */
    Package *(*find_pkg)(PackageMap *, const char *, const char *) = package_map_find;

    printf("\nDetected conflicts for %s/%s (latest valid version %d.%d.%d):\n",
           author, name, version->major, version->minor, version->patch);

    int reported = 0;
    for (size_t i = 0; i < version->dependency_count; i++) {
        V2Dependency *dep = &version->dependencies[i];
        const char *slash = strchr(dep->package_name, '/');
        if (!slash) {
            continue;
        }
        size_t dep_author_len = (size_t)(slash - dep->package_name);
        char *dep_author = arena_malloc(dep_author_len + 1);
        if (!dep_author) {
            continue;
        }
        memcpy(dep_author, dep->package_name, dep_author_len);
        dep_author[dep_author_len] = '\0';
        const char *dep_name = slash + 1;

        /* Find current pinned version in any section */
        Package *cur = NULL;
        PackageMap *maps[4] = {
            elm_json->dependencies_direct,
            elm_json->dependencies_indirect,
            elm_json->dependencies_test_direct,
            elm_json->dependencies_test_indirect
        };
        for (int m = 0; m < 4 && !cur; m++) {
            cur = find_pkg(maps[m], dep_author, dep_name);
        }

        if (cur) {
            PgVersionRange constraint;
            if (pg_elm_v2_parse_constraint(dep->constraint, &constraint)) {
                PgVersion cur_v;
                if (pg_version_parse(cur->version, &cur_v) && !pg_range_contains(constraint, cur_v)) {
                    printf("  - %s/%s requires %s but project pins %s\n",
                           dep_author, dep_name, dep->constraint, cur->version);
                    reported++;
                }
            } else {
                /* Fallback: show raw constraint text */
                printf("  - %s/%s requires %s but project pins %s\n",
                       dep_author, dep_name, dep->constraint, cur->version);
                reported++;
            }
        }

        arena_free(dep_author);
    }

    if (reported == 0) {
        printf("  (no pinned dependencies found to compare; constraints are incompatible with available versions)\n");
    }
    printf("\n");
}

int cmd_debug_install_plan(int argc, char *argv[]) {
    if (argc < 2) {
        print_install_plan_usage();
        return 1;
    }

    const char *package = argv[1];

    if (strcmp(package, "-h") == 0 || strcmp(package, "--help") == 0) {
        print_install_plan_usage();
        return 0;
    }

    bool is_test = false;
    bool major_upgrade = false;
    bool quiet_mode = false;

    // Parse options
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_install_plan_usage();
            return 0;
        } else if (strcmp(argv[i], "--test") == 0) {
            is_test = true;
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            // Already handled by main
        } else if (strcmp(argv[i], "-vv") == 0) {
            // Already handled by main
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet_mode = true;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_install_plan_usage();
            return 1;
        }
    }

    // Default to verbose mode unless quiet or already verbose
    if (!quiet_mode && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    // Parse package name
    const char *slash = strchr(package, '/');
    if (!slash) {
        fprintf(stderr, "Error: Package name must be in author/name format (e.g., elm/html)\n");
        return 1;
    }

    size_t author_len = slash - package;
    char *author = arena_malloc(author_len + 1);
    char *name = arena_strdup(slash + 1);
    if (!author || !name) {
        fprintf(stderr, "Error: Out of memory\n");
        return 1;
    }
    memcpy(author, package, author_len);
    author[author_len] = '\0';

    // Find elm.json
    char *elm_json_path = find_elm_json();
    if (!elm_json_path) {
        fprintf(stderr, "Error: Could not find elm.json in current directory or parent directories\n");
        return 1;
    }

    // Load elm.json
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        fprintf(stderr, "Error: Failed to load elm.json from %s\n", elm_json_path);
        arena_free(elm_json_path);
        return 1;
    }

    // Initialize global context
    global_context_init();

    // Create install environment
    InstallEnv *install_env = install_env_create();
    if (!install_env) {
        fprintf(stderr, "Error: Failed to create install environment\n");
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        return 1;
    }

    if (!install_env_init(install_env)) {
        fprintf(stderr, "Error: Failed to initialize install environment\n");
        install_env_free(install_env);
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        return 1;
    }

    // Initialize solver
    SolverState *solver = solver_init(install_env, true); // online=true
    if (!solver) {
        fprintf(stderr, "Error: Failed to initialize solver\n");
        install_env_free(install_env);
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        return 1;
    }

    // Run solver to get install plan
    InstallPlan *plan = NULL;
    SolverResult result = solver_add_package(solver, elm_json, author, name, is_test, major_upgrade, &plan);

    if (result != SOLVER_OK) {
        /* If V2, try to spell out obvious pinned-version conflicts for the target package */
        report_conflicts_v2(install_env, elm_json, author, name);

        printf("\n");
        printf("Failed to create install plan for package %s/%s%s%s\n", author, name,
               is_test ? " (test dependency)" : "",
               major_upgrade ? " (major upgrades allowed)" : "");
        printf("\n");
        printf("Reason: ");
        switch (result) {
            case SOLVER_NO_SOLUTION:
                printf("No solution found - the package has conflicts with current dependencies\n");
                break;
            case SOLVER_NO_OFFLINE_SOLUTION:
                printf("No offline solution found - network connection may be required\n");
                break;
            case SOLVER_NETWORK_ERROR:
                printf("Network error occurred\n");
                break;
            case SOLVER_INVALID_PACKAGE:
                printf("Invalid package name or package does not exist\n");
                break;
            default:
                printf("Unknown error\n");
                break;
        }
        printf("\n");
        printf("See error messages above for details about conflicts.\n");
    } else {
        printf("Install plan for package %s/%s%s%s:\n", author, name,
               is_test ? " (test dependency)" : "",
               major_upgrade ? " (major upgrades allowed)" : "");
        printf("\n");

        if (!plan || plan->count == 0) {
            printf("No packages need to be installed\n");
        } else {
            printf("Packages to be installed:\n");
            for (int i = 0; i < plan->count; i++) {
                PackageChange *change = &plan->changes[i];
                if (change->old_version) {
                    printf("  %s/%s: %s -> %s\n", change->author, change->name,
                           change->old_version, change->new_version);
                } else {
                    printf("  %s/%s: %s (new)\n", change->author, change->name,
                           change->new_version);
                }
            }
        }
    }

    // Cleanup
    if (plan) {
        install_plan_free(plan);
    }
    solver_free(solver);
    install_env_free(install_env);
    elm_json_free(elm_json);
    arena_free(elm_json_path);

    return (result == SOLVER_OK) ? 0 : 1;
}
