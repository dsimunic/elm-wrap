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
    printf("  --local-dev        Debug local development package installation\n");
    printf("  --from-path <path> Path to local package (requires --local-dev)\n");
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

    const char *package = NULL;

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
            if (!package) {
                package = argv[i];
            } else {
                fprintf(stderr, "Error: Multiple package names specified\n");
                return 1;
            }
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
        const char *slash = strchr(pkg_json->package_name, '/');
        if (!slash) {
            fprintf(stderr, "Error: Invalid package name in elm.json: %s\n", pkg_json->package_name);
            elm_json_free(pkg_json);
            return 1;
        }
        
        size_t author_len = slash - pkg_json->package_name;
        author = arena_malloc(author_len + 1);
        name = arena_strdup(slash + 1);
        if (!author || !name) {
            fprintf(stderr, "Error: Out of memory\n");
            elm_json_free(pkg_json);
            return 1;
        }
        memcpy(author, pkg_json->package_name, author_len);
        author[author_len] = '\0';
        
        // Verify package name matches if specified
        if (package) {
            char full_name[256];
            snprintf(full_name, sizeof(full_name), "%s/%s", author, name);
            if (strcmp(package, full_name) != 0) {
                fprintf(stderr, "Error: Package name mismatch: specified %s but elm.json has %s\n", 
                        package, full_name);
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
        
        // Initialize global context and environment
        global_context_init();
        
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
                    SolverState *solver = solver_init(install_env, true);
                    if (solver) {
                        InstallPlan *dep_plan = NULL;
                        SolverResult result = solver_add_package(solver, app_json, dep->author, dep->name, 
                                                                  is_test, false, &dep_plan);
                        
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
        // Regular mode - require package name
        if (!package) {
            fprintf(stderr, "Error: Package name required\n");
            print_install_plan_usage();
            return 1;
        }
        
        // Parse package name
        const char *slash = strchr(package, '/');
        if (!slash) {
            fprintf(stderr, "Error: Package name must be in author/name format (e.g., elm/html)\n");
            return 1;
        }

        size_t author_len = slash - package;
        author = arena_malloc(author_len + 1);
        name = arena_strdup(slash + 1);
        if (!author || !name) {
            fprintf(stderr, "Error: Out of memory\n");
            return 1;
        }
        memcpy(author, package, author_len);
        author[author_len] = '\0';
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
