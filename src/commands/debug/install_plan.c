#include "debug.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../dyn_array.h"
#include "../../log.h"
#include "../../solver.h"
#include "../../install_env.h"
#include "../../elm_json.h"
#include "../../fileutil.h"
#include "../../global_context.h"
#include "../../terminal_colors.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../protocol_v2/solver/pg_elm_v2.h"
#include "../../pgsolver/pg_core.h"
#include "../package/package_common.h"

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
    printf("Usage: %s debug install-plan PACKAGE [PACKAGE ...] [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Show what packages would be installed for one or more packages (dry-run).\n");
    printf("This exercises the dependency solver without actually installing anything.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PACKAGE           Package name in author/name format (e.g., elm/html)\n");
    printf("                     Multiple packages can be specified.\n");
    printf("\n");
    printf("Options:\n");
    printf("  --test             Show plan for test dependencies\n");
    printf("  --major            Allow major version upgrades (single package only)\n");
    printf("  --local-dev        Debug local development package installation\n");
    printf("  --from-path PATH   Path to local package (requires --local-dev)\n");
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
        char *dep_author = NULL;
        char *dep_name = NULL;
        if (!parse_package_name(dep->package_name, &dep_author, &dep_name)) {
            continue;
        }

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
            if (version_parse_constraint(dep->constraint, &constraint)) {
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

    /* Multi-package support: collect package names into a dynamic array */
    const char **packages = NULL;
    int package_count = 0;
    int package_capacity = 0;

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_install_plan_usage();
        return 0;
    }

    bool is_test = false;
    bool major_upgrade = false;
    bool quiet_mode = false;
    bool local_dev = false;
    const char *from_path = NULL;

    // Parse options - first pass to extract flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_install_plan_usage();
            return 0;
        } else if (strcmp(argv[i], "--test") == 0) {
            is_test = true;
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
        } else if (strcmp(argv[i], "--local-dev") == 0) {
            local_dev = true;
        } else if (strcmp(argv[i], "--from-path") == 0) {
            if (i + 1 < argc) {
                i++;
                from_path = argv[i];
            } else {
                fprintf(stderr, "Error: --from-path requires a path argument\n");
                print_install_plan_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            // Already handled by main
        } else if (strcmp(argv[i], "-vv") == 0) {
            // Already handled by main
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet_mode = true;
        } else if (argv[i][0] != '-') {
            /* Collect package names into array */
            DYNARRAY_PUSH(packages, package_count, package_capacity, argv[i], const char*);
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_install_plan_usage();
            return 1;
        }
    }

    // Validate options
    if (from_path && !local_dev) {
        fprintf(stderr, "Error: --from-path requires --local-dev flag\n");
        print_install_plan_usage();
        return 1;
    }

    /* --major only works with single package */
    if (major_upgrade && package_count > 1) {
        fprintf(stderr, "Error: --major can only be used with a single package\n");
        return 1;
    }

    // Default to verbose mode unless quiet or already verbose
    if (!quiet_mode && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    // For local-dev, we can derive package name from local elm.json if not specified
    char *author = NULL;
    char *name = NULL;
    
    if (local_dev) {
        const char *source_path = from_path ? from_path : ".";
        char source_elm_json[1024];
        snprintf(source_elm_json, sizeof(source_elm_json), "%s/elm.json", source_path);
        
        if (!file_exists(source_elm_json)) {
            fprintf(stderr, "Error: No elm.json found in source directory: %s\n", source_path);
            return 1;
        }
        
        // Read package info from local elm.json
        ElmJson *pkg_json = elm_json_read(source_elm_json);
        if (!pkg_json) {
            fprintf(stderr, "Error: Failed to read %s\n", source_elm_json);
            return 1;
        }
        
        if (pkg_json->type != ELM_PROJECT_PACKAGE) {
            fprintf(stderr, "Error: %s is not a package project\n", source_elm_json);
            elm_json_free(pkg_json);
            return 1;
        }
        
        if (!pkg_json->package_name) {
            fprintf(stderr, "Error: No package name in %s\n", source_elm_json);
            elm_json_free(pkg_json);
            return 1;
        }
        
        // Parse package name from elm.json
        if (!parse_package_name(pkg_json->package_name, &author, &name)) {
            fprintf(stderr, "Error: Invalid package name in elm.json: %s\n", pkg_json->package_name);
            elm_json_free(pkg_json);
            return 1;
        }
        
        // Verify package name matches if specified
        if (package_count > 0) {
            char full_name[MAX_PACKAGE_NAME_LENGTH];
            snprintf(full_name, sizeof(full_name), "%s/%s", author, name);
            if (strcmp(packages[0], full_name) != 0) {
                fprintf(stderr, "Error: Package name mismatch: specified %s but elm.json has %s\n", 
                        packages[0], full_name);
                elm_json_free(pkg_json);
                return 1;
            }
        }
        
        printf("\n=== Local Development Package Debug ===\n");
        printf("Source path: %s\n", source_path);
        printf("Package: %s/%s\n", author, name);
        printf("Version in elm.json: %s\n", pkg_json->package_version ? pkg_json->package_version : "unknown");
        printf("\nDependencies from local elm.json:\n");
        
        if (pkg_json->package_dependencies && pkg_json->package_dependencies->count > 0) {
            for (int i = 0; i < pkg_json->package_dependencies->count; i++) {
                Package *dep = &pkg_json->package_dependencies->packages[i];
                printf("  %s/%s: %s\n", dep->author, dep->name, dep->version ? dep->version : "any");
            }
        } else {
            printf("  (none)\n");
        }
        printf("\n");
        
        // Find app's elm.json
        char *elm_json_path = find_elm_json();
        if (!elm_json_path) {
            fprintf(stderr, "Error: Could not find elm.json in current directory or parent directories\n");
            elm_json_free(pkg_json);
            return 1;
        }
        
        ElmJson *app_json = elm_json_read(elm_json_path);
        if (!app_json) {
            fprintf(stderr, "Error: Failed to load elm.json from %s\n", elm_json_path);
            elm_json_free(pkg_json);
            arena_free(elm_json_path);
            return 1;
        }
        
        printf("Target app: %s\n\n", elm_json_path);

        InstallEnv *install_env = install_env_create();
        if (!install_env) {
            fprintf(stderr, "Error: Failed to create install environment\n");
            elm_json_free(pkg_json);
            elm_json_free(app_json);
            arena_free(elm_json_path);
            return 1;
        }
        
        if (!install_env_init(install_env)) {
            fprintf(stderr, "Error: Failed to initialize install environment\n");
            install_env_free(install_env);
            elm_json_free(pkg_json);
            elm_json_free(app_json);
            arena_free(elm_json_path);
            return 1;
        }
        
        printf("=== Dependency Resolution Analysis ===\n\n");
        
        // Check if main package is in registry
        bool main_in_registry = false;
        if (install_env->protocol_mode == PROTOCOL_V2 && install_env->v2_registry) {
            main_in_registry = v2_registry_find(install_env->v2_registry, author, name) != NULL;
        } else if (install_env->registry) {
            main_in_registry = registry_find(install_env->registry, author, name) != NULL;
        }
        
        printf("Main package %s/%s:\n", author, name);
        if (main_in_registry) {
            printf("  Status: EXISTS in registry (local version 999.0.0 would be used)\n");
        } else {
            printf("  Status: NOT in registry (local version 0.0.0 would be used)\n");
        }
        printf("\n");
        
        // Check each dependency
        int issues = 0;
        if (pkg_json->package_dependencies && pkg_json->package_dependencies->count > 0) {
            printf("Dependency analysis:\n");
            
            for (int i = 0; i < pkg_json->package_dependencies->count; i++) {
                Package *dep = &pkg_json->package_dependencies->packages[i];
                
                bool dep_in_registry = false;
                if (install_env->protocol_mode == PROTOCOL_V2 && install_env->v2_registry) {
                    dep_in_registry = v2_registry_find(install_env->v2_registry, dep->author, dep->name) != NULL;
                } else if (install_env->registry) {
                    dep_in_registry = registry_find(install_env->registry, dep->author, dep->name) != NULL;
                }
                
                printf("\n  %s/%s (constraint: %s):\n", dep->author, dep->name, 
                       dep->version ? dep->version : "any");
                
                if (!dep_in_registry) {
                    printf("    Status: NOT IN REGISTRY\n");
                    printf("    Action: Must also install with --local-dev\n");
                    issues++;
                } else {
                    printf("    Status: Available in registry\n");
                    
                    // Try to resolve this dependency
                    SolverState *solver = solver_init(install_env, install_env_solver_online(install_env));
                    if (solver) {
                        InstallPlan *dep_plan = NULL;
                        SolverResult result = solver_add_package(solver, app_json, dep->author, dep->name,
                                                                  NULL, is_test, false, false, &dep_plan);
                        
                        if (result == SOLVER_OK) {
                            printf("    Resolution: OK\n");
                            if (dep_plan && dep_plan->count > 0) {
                                printf("    Would install:\n");
                                for (int j = 0; j < dep_plan->count; j++) {
                                    PackageChange *change = &dep_plan->changes[j];
                                    if (change->old_version) {
                                        printf("      %s/%s: %s -> %s\n", 
                                               change->author, change->name,
                                               change->old_version, change->new_version);
                                    } else {
                                        printf("      %s/%s: %s (new)\n", 
                                               change->author, change->name,
                                               change->new_version);
                                    }
                                }
                            } else {
                                printf("    Would install: (already satisfied)\n");
                            }
                        } else {
                            printf("    Resolution: FAILED\n");
                            printf("    Reason: ");
                            switch (result) {
                                case SOLVER_NO_SOLUTION:
                                    printf("Conflicts with current dependencies\n");
                                    break;
                                case SOLVER_INVALID_PACKAGE:
                                    printf("Invalid package or version constraint\n");
                                    break;
                                default:
                                    printf("Solver error\n");
                                    break;
                            }
                            issues++;
                        }
                        
                        if (dep_plan) install_plan_free(dep_plan);
                        solver_free(solver);
                    } else {
                        printf("    Resolution: Could not initialize solver\n");
                        issues++;
                    }
                }
            }
        }
        
        printf("\n=== Summary ===\n");
        if (issues == 0) {
            printf("All dependencies can be resolved. Local-dev installation should succeed.\n");
        } else {
            printf("Found %d issue(s) that would prevent installation.\n", issues);
        }
        
        elm_json_free(pkg_json);
        elm_json_free(app_json);
        install_env_free(install_env);
        arena_free(elm_json_path);
        return issues > 0 ? 1 : 0;
    } else {
        // Regular mode - require at least one package name
        if (package_count == 0) {
            fprintf(stderr, "Error: At least one package name required\n");
            print_install_plan_usage();
            return 1;
        }
    }

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
    SolverState *solver = solver_init(install_env, install_env_solver_online(install_env));
    if (!solver) {
        fprintf(stderr, "Error: Failed to initialize solver\n");
        install_env_free(install_env);
        elm_json_free(elm_json);
        arena_free(elm_json_path);
        return 1;
    }

    // Run solver to get install plan
    InstallPlan *plan = NULL;
    SolverResult result;

    if (package_count == 1) {
        // Single package mode - use original logic for better error messages
        const char *package = packages[0];
        if (!parse_package_name(package, &author, &name)) {
            fprintf(stderr, "Error: Package name must be in author/name format (e.g., elm/html)\n");
            solver_free(solver);
            install_env_free(install_env);
            elm_json_free(elm_json);
            arena_free(elm_json_path);
            return 1;
        }

        result = solver_add_package(solver, elm_json, author, name, NULL, is_test, major_upgrade, false, &plan);

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
    } else {
        // Multi-package mode - convert package names to PackageVersionSpec
        PackageVersionSpec *specs = arena_malloc(package_count * sizeof(PackageVersionSpec));
        for (int i = 0; i < package_count; i++) {
            char *pkg_author = NULL;
            char *pkg_name = NULL;
            if (!parse_package_name(packages[i], &pkg_author, &pkg_name)) {
                fprintf(stderr, "Error: Invalid package name '%s'\n", packages[i]);
                solver_free(solver);
                install_env_free(install_env);
                elm_json_free(elm_json);
                arena_free(elm_json_path);
                return 1;
            }
            specs[i].author = pkg_author;
            specs[i].name = pkg_name;
            specs[i].version = NULL;  // No version targeting in debug command
        }

        MultiPackageValidation *validation = NULL;
        result = solver_add_packages(solver, elm_json, specs, package_count, is_test, false, &plan, &validation);

        // Print validation errors if any
        if (validation && validation->invalid_count > 0) {
            printf("\n");
            printf("Package validation errors:\n");
            for (int i = 0; i < validation->count; i++) {
                PackageValidationResult *r = &validation->results[i];
                if (!r->valid_name || !r->exists) {
                    printf("  %s✗%s %s/%s: %s\n", ANSI_RED, ANSI_RESET,
                           r->author ? r->author : "?",
                           r->name ? r->name : "?",
                           r->error_msg ? r->error_msg : "Unknown error");
                }
            }
            printf("\n");
        }

        if (result != SOLVER_OK) {
            printf("Failed to create install plan for packages%s\n",
                   is_test ? " (test dependencies)" : "");
            printf("\n");
            printf("Reason: ");
            switch (result) {
                case SOLVER_NO_SOLUTION:
                    printf("No solution found - the packages have conflicts with current dependencies\n");
                    break;
                case SOLVER_NO_OFFLINE_SOLUTION:
                    printf("No offline solution found - network connection may be required\n");
                    break;
                case SOLVER_NETWORK_ERROR:
                    printf("Network error occurred\n");
                    break;
                case SOLVER_INVALID_PACKAGE:
                    printf("One or more packages are invalid or do not exist\n");
                    break;
                default:
                    printf("Unknown error\n");
                    break;
            }
            printf("\n");
        } else {
            printf("Install plan for %d packages%s:\n", package_count,
                   is_test ? " (test dependencies)" : "");
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

        if (validation) {
            multi_package_validation_free(validation);
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
