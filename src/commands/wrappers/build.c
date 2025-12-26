/**
 * build.c - Build command implementation
 *
 * Generates a JSON build plan for Elm compilation.
 * Subcommands:
 *   check - Display human-readable build plan and prompt to run make
 */

#include "build.h"
#include "make.h"
#include "../../build/build_driver.h"
#include "../../elm_json.h"
#include "../../global_context.h"
#include "../../install_env.h"
#include "../../fileutil.h"
#include "../../alloc.h"
#include "../../shared/log.h"
#include "../../shared/package_list.h"
#include "../../constants.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <ctype.h>

static void print_build_usage(void) {
    const char *prog = global_context_program_name();
    printf("Usage: %s build [SUBCOMMAND] [OPTIONS] PATH [PATH...]\n", prog);
    printf("\n");
    printf("Generate and analyze build plans for Elm compilation.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  check              Display human-readable build plan and confirm before building\n");
    printf("  (none)             Output JSON build plan (default)\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PATH               Entry point Elm file(s) (e.g., src/Main.elm)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --json             Output as JSON (default, for no subcommand)\n");
    printf("  -q, --quiet        Suppress progress messages to stderr\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s build src/Main.elm\n", prog);
    printf("  %s build check src/Main.elm\n", prog);
    printf("  %s build src/Main.elm src/Worker.elm\n", prog);
}

static void print_build_check_usage(void) {
    const char *prog = global_context_program_name();
    printf("Usage: %s build check [OPTIONS] PATH [PATH...]\n", prog);
    printf("\n");
    printf("Analyze the project, display a human-readable build plan, and\n");
    printf("optionally proceed with compilation.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PATH               Entry point Elm file(s) (e.g., src/Main.elm)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes          Skip confirmation prompt and proceed with build\n");
    printf("  -n, --no           Show plan only, do not prompt or build\n");
    printf("  -q, --quiet        Suppress progress messages to stderr\n");
    printf("  -h, --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s build check src/Main.elm\n", prog);
    printf("  %s build check -y src/Main.elm\n", prog);
}

/* ============================================================================
 * Helper: count modules per source directory
 * ========================================================================== */

typedef struct {
    const char *src_dir;       /* Source directory (relative from project root) */
    int module_count;          /* Number of modules in this directory */
} SrcDirStats;

static void compute_src_dir_stats(BuildPlan *plan, SrcDirStats **out_stats,
                                  int *out_count) {
    /* Allocate stats array (one per source directory) */
    int stats_capacity = plan->src_dir_count > 0 ? plan->src_dir_count : 1;
    SrcDirStats *stats = arena_calloc(stats_capacity, sizeof(SrcDirStats));
    int stats_count = 0;

    /* Initialize with source directories */
    for (int i = 0; i < plan->src_dir_count; i++) {
        /* Extract relative path from absolute */
        const char *src_dir = plan->src_dirs[i];
        const char *rel_path = src_dir;

        /* If starts with project root, make it relative */
        if (plan->root && strncmp(src_dir, plan->root, strlen(plan->root)) == 0) {
            rel_path = src_dir + strlen(plan->root);
            if (*rel_path == '/') rel_path++;
        }
        if (*rel_path == '\0') rel_path = ".";

        stats[stats_count].src_dir = rel_path;
        stats[stats_count].module_count = 0;
        stats_count++;
    }

    /* Count modules per source directory */
    for (int m = 0; m < plan->module_count; m++) {
        const char *mod_path = plan->modules[m].path;
        if (!mod_path) continue;

        /* Find which source directory this module belongs to */
        for (int s = 0; s < stats_count; s++) {
            const char *src = stats[s].src_dir;
            size_t src_len = strlen(src);

            /* Check if module path starts with this source directory */
            if (strncmp(mod_path, src, src_len) == 0 &&
                (mod_path[src_len] == '/' || mod_path[src_len] == '\0')) {
                stats[s].module_count++;
                break;
            }
        }
    }

    *out_stats = stats;
    *out_count = stats_count;
}

/* ============================================================================
 * cmd_build_check - Human-readable build plan with confirmation
 * ========================================================================== */

int cmd_build_check(int argc, char *argv[]) {
    /* Options */
    bool auto_yes = false;
    bool auto_no = false;
    bool quiet = false;

    /* Collect entry files */
    int entry_capacity = 8;
    int entry_count = 0;
    const char **entry_files = arena_malloc(entry_capacity * sizeof(char*));

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_build_check_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no") == 0) {
            auto_no = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (argv[i][0] != '-') {
            /* Entry file */
            if (entry_count >= entry_capacity) {
                entry_capacity *= 2;
                entry_files = arena_realloc(entry_files, entry_capacity * sizeof(char*));
            }
            entry_files[entry_count++] = argv[i];
        } else {
            log_error("Unknown option: %s", argv[i]);
            print_build_check_usage();
            return 1;
        }
    }

    if (auto_yes && auto_no) {
        log_error("Cannot specify both -y and -n");
        return 1;
    }

    if (entry_count == 0) {
        log_error("No entry file specified");
        print_build_check_usage();
        return 1;
    }

    /* Find elm.json by walking up from the first entry file */
    char *elm_json_path = find_elm_json_upwards(entry_files[0]);
    if (!elm_json_path) {
        log_error("Could not find elm.json starting from: %s", entry_files[0]);
        log_error("Please run this command from within an Elm project directory.");
        return 1;
    }

    /* Extract project root directory from elm.json path */
    char *project_root = arena_strdup(elm_json_path);
    char *last_slash = strrchr(project_root, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        project_root = arena_strdup(".");
    }

    if (!quiet) {
        log_debug("Project root: %s", project_root);
        log_debug("elm.json: %s", elm_json_path);
    }

    /* Read elm.json */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_error("Failed to read elm.json at: %s", elm_json_path);
        return 1;
    }

    /* Initialize install environment (for registry/solver access) */
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

    if (!quiet) {
        log_debug("ELM_HOME: %s", env->cache->elm_home);
    }

    /* Generate build plan */
    BuildPlan *plan = build_generate_plan(
        project_root,
        elm_json,
        env,
        entry_files,
        entry_count
    );

    if (!plan) {
        log_error("Failed to generate build plan");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Display human-readable build plan */
    printf("\n");
    printf("---- Build Plan ");
    for (int i = 0; i < 50; i++) printf("-");
    printf("\n\n");

    /* Entry points */
    printf("Build plan for: ");
    for (int i = 0; i < entry_count; i++) {
        if (i > 0) printf(", ");
        printf("`%s`", entry_files[i]);
    }
    printf("\n\n");

    /* Check for problems first */
    if (plan->problem_count > 0) {
        printf("PROBLEMS DETECTED:\n\n");
        for (int i = 0; i < plan->problem_count; i++) {
            if (plan->problems[i].module_name) {
                printf("  - %s: %s\n", plan->problems[i].module_name,
                       plan->problems[i].message);
            } else {
                printf("  - %s\n", plan->problems[i].message);
            }
        }
        printf("\n");
    }

    /* Package summary */
    if (plan->packages_with_artifacts > 0) {
        printf("Include cached data for %d already built package%s.\n",
               plan->packages_with_artifacts,
               plan->packages_with_artifacts == 1 ? "" : "s");
    }

    /* Packages needing rebuild (stale or missing) */
    int packages_to_rebuild = plan->packages_stale + plan->packages_missing;
    if (packages_to_rebuild > 0) {
        printf("\n");
        printf("Rebuild %d package%s that %s out of date:\n",
               packages_to_rebuild,
               packages_to_rebuild == 1 ? "" : "s",
               packages_to_rebuild == 1 ? "is" : "are");

        /* Build a list of packages to print */
        PackageListEntry *rebuild_entries = arena_malloc(
            (size_t)packages_to_rebuild * sizeof(PackageListEntry));
        int rebuild_count = 0;

        if (rebuild_entries) {
            for (int i = 0; i < plan->package_count; i++) {
                BuildPackage *pkg = &plan->packages[i];
                if (pkg->artifact_status == ARTIFACT_STALE ||
                    pkg->artifact_status == ARTIFACT_MISSING) {
                    /* Parse "author/name" format */
                    const char *slash = strchr(pkg->name, '/');
                    if (slash) {
                        size_t author_len = (size_t)(slash - pkg->name);
                        char *author = arena_malloc(author_len + 1);
                        if (author) {
                            memcpy(author, pkg->name, author_len);
                            author[author_len] = '\0';

                            rebuild_entries[rebuild_count].author = author;
                            rebuild_entries[rebuild_count].name = slash + 1;
                            rebuild_entries[rebuild_count].version = pkg->version;

                            /* Check if this is a local-dev package */
                            if (pkg->version && strstr(pkg->version, "local-dev")) {
                                rebuild_entries[rebuild_count].annotation = " (local-dev)";
                            } else {
                                rebuild_entries[rebuild_count].annotation = NULL;
                            }
                            rebuild_count++;
                        }
                    }
                }
            }

            /* Print sorted package list */
            printf("\n");
            package_list_print_sorted(rebuild_entries, rebuild_count, 0, 2);

            /* Free author strings */
            for (int i = 0; i < rebuild_count; i++) {
                arena_free((void *)rebuild_entries[i].author);
            }
            arena_free(rebuild_entries);
        }
    }

    /* Module summary by source directory */
    if (plan->module_count > 0) {
        SrcDirStats *stats = NULL;
        int stats_count = 0;
        compute_src_dir_stats(plan, &stats, &stats_count);

        printf("\n");
        printf("Build %d module%s from the source path%s:\n\n",
               plan->module_count,
               plan->module_count == 1 ? "" : "s",
               stats_count == 1 ? "" : "s");

        /* Find the longest source directory name for alignment */
        int max_len = 0;
        for (int i = 0; i < stats_count; i++) {
            int len = (int)strlen(stats[i].src_dir);
            if (len > max_len) max_len = len;
        }

        for (int i = 0; i < stats_count; i++) {
            if (stats[i].module_count > 0) {
                printf("  %-*s: %3d module%s\n",
                       max_len + 1, stats[i].src_dir,
                       stats[i].module_count,
                       stats[i].module_count == 1 ? "" : "s");
            }
        }
    }

    printf("\n");

    /* If there are problems, don't offer to build */
    if (plan->problem_count > 0) {
        printf("Cannot proceed with build due to problems above.\n\n");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Cleanup before potential exec */
    install_env_free(env);
    elm_json_free(elm_json);

    /* Handle auto modes */
    if (auto_no) {
        return 0;
    }

    bool proceed = auto_yes;
    if (!proceed) {
        /* Ask for confirmation */
        printf("Do you want to proceed with build? [Y/n] ");
        fflush(stdout);

        int c = getchar();
        if (c == '\n' || c == 'Y' || c == 'y') {
            proceed = true;
        } else if (c == 'N' || c == 'n') {
            proceed = false;
            /* Consume rest of line */
            while (c != '\n' && c != EOF) c = getchar();
        } else {
            /* Consume rest of line */
            while (c != '\n' && c != EOF) c = getchar();
            proceed = false;
        }
    }

    if (!proceed) {
        printf("Build cancelled.\n");
        return 0;
    }

    printf("\n");

    /* Build arguments for cmd_make */
    /* argv[0] was "check", we need to pass the entry files to make */
    int make_argc = entry_count + 1;
    char **make_argv = arena_malloc((make_argc + 1) * sizeof(char*));
    make_argv[0] = "make";
    for (int i = 0; i < entry_count; i++) {
        make_argv[i + 1] = (char*)entry_files[i];
    }
    make_argv[make_argc] = NULL;

    /* Call cmd_make - this will execve and not return on success */
    return cmd_make(make_argc, make_argv);
}

int cmd_build(int argc, char *argv[]) {
    /* Check for subcommand */
    if (argc >= 2 && argv[1][0] != '-') {
        if (strcmp(argv[1], "check") == 0) {
            return cmd_build_check(argc - 1, argv + 1);
        }
        /* If it's not a known subcommand and doesn't look like a flag,
         * treat it as an entry file (fall through to normal processing) */
    }
    /* Collect entry files */
    int entry_capacity = 8;
    int entry_count = 0;
    const char **entry_files = arena_malloc(entry_capacity * sizeof(char*));
    bool quiet = false;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_build_usage();
            return 0;
        } else if (strcmp(argv[i], "--json") == 0) {
            /* Default behavior, ignore */
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (argv[i][0] != '-') {
            /* Entry file */
            if (entry_count >= entry_capacity) {
                entry_capacity *= 2;
                entry_files = arena_realloc(entry_files, entry_capacity * sizeof(char*));
            }
            entry_files[entry_count++] = argv[i];
        } else {
            log_error("Unknown option: %s", argv[i]);
            print_build_usage();
            return 1;
        }
    }

    if (entry_count == 0) {
        log_error("No entry file specified");
        print_build_usage();
        return 1;
    }

    /* Find elm.json by walking up from the first entry file */
    char *elm_json_path = find_elm_json_upwards(entry_files[0]);
    if (!elm_json_path) {
        log_error("Could not find elm.json starting from: %s", entry_files[0]);
        log_error("Please run this command from within an Elm project directory.");
        return 1;
    }

    /* Extract project root directory from elm.json path */
    char *project_root = arena_strdup(elm_json_path);
    char *last_slash = strrchr(project_root, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        project_root = arena_strdup(".");
    }

    if (!quiet) {
        log_debug("Project root: %s", project_root);
        log_debug("elm.json: %s", elm_json_path);
    }

    /* Read elm.json */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_error("Failed to read elm.json at: %s", elm_json_path);
        return 1;
    }

    /* Initialize install environment (for registry/solver access) */
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

    if (!quiet) {
        log_debug("ELM_HOME: %s", env->cache->elm_home);
    }

    /* Generate build plan */
    BuildPlan *plan = build_generate_plan(
        project_root,
        elm_json,
        env,
        entry_files,
        entry_count
    );

    if (!plan) {
        log_error("Failed to generate build plan");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Output JSON */
    char *json = build_plan_to_json(plan);
    if (json) {
        printf("%s\n", json);
    } else {
        log_error("Failed to generate JSON output");
        install_env_free(env);
        elm_json_free(elm_json);
        return 1;
    }

    /* Cleanup */
    install_env_free(env);
    elm_json_free(elm_json);

    /* Return failure if there were problems */
    return (plan->problem_count > 0) ? 1 : 0;
}
