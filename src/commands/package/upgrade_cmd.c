/**
 * upgrade_cmd.c - Package Upgrade Command Dispatcher
 *
 * Entry point for the package upgrade command.
 * Delegates to V1 or V2 protocol-specific implementations based on global context.
 */

#include "package_common.h"
#include "upgrade_v1.h"
#include "upgrade_v2.h"
#include "../../install.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../install_env.h"
#include "../../shared/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void print_upgrade_usage(void) {
    printf("Usage: %s package upgrade [PACKAGE|all]\n", global_context_program_name());
    printf("\n");
    printf("Upgrade packages to their latest available versions.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s package upgrade                        # Upgrade all packages to latest minor versions\n", global_context_program_name());
    printf("  %s package upgrade all                    # Same as above\n", global_context_program_name());
    printf("  %s package upgrade elm/html               # Upgrade elm/html to latest minor version\n", global_context_program_name());
    printf("  %s package upgrade --major elm/html       # Upgrade elm/html to latest major version\n", global_context_program_name());
    printf("  %s package upgrade --major all            # Upgrade all packages to latest major versions\n", global_context_program_name());
    printf("  %s package upgrade --major-ignore-test elm/html # Major upgrade, ignore test deps\n", global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  --major                              # Allow major version upgrades\n");
    printf("  --major-ignore-test                  # Allow major upgrades, ignore test dependency conflicts\n");
    printf("  -y, --yes                            # Automatically confirm changes\n");
    printf("  -v, --verbose                        # Show progress reports (registry, connectivity)\n");
    printf("  -q, --quiet                          # Suppress progress reports\n");
    printf("  --help                               # Show this help\n");
}

int cmd_upgrade(int argc, char *argv[]) {
    bool major_upgrade = false;
    bool major_ignore_test = false;
    bool auto_yes = false;
    bool cmd_verbose = false;
    bool cmd_quiet = false;
    const char *package_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_upgrade_usage();
            return 0;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            auto_yes = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cmd_verbose = true;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            cmd_quiet = true;
        } else if (strcmp(argv[i], "--major-ignore-test") == 0) {
            major_upgrade = true;
            major_ignore_test = true;
        } else if (strcmp(argv[i], "--major") == 0) {
            major_upgrade = true;
        } else if (argv[i][0] != '-') {
            if (package_name) {
                fprintf(stderr, "Error: Multiple package names specified\n");
                return 1;
            }
            package_name = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option: %s\n", argv[i]);
            print_upgrade_usage();
            return 1;
        }
    }

    bool upgrade_all = false;
    if (!package_name || strcmp(package_name, "all") == 0) {
        upgrade_all = true;
    }

    LogLevel original_level = g_log_level;
    if (cmd_quiet) {
        if (g_log_level >= LOG_LEVEL_PROGRESS) {
            log_set_level(LOG_LEVEL_WARN);
        }
    } else if (cmd_verbose && !log_is_progress()) {
        log_set_level(LOG_LEVEL_PROGRESS);
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        log_set_level(original_level);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        install_env_free(env);
        log_set_level(original_level);
        return 1;
    }

    int result = 0;

    /* Protocol switching: use V2 if available, otherwise V1 */
    if (global_context_is_v2() && env->v2_registry) {
        log_debug("Using V2 protocol for upgrade");
        if (upgrade_all) {
            result = upgrade_all_packages_v2(elm_json, env, major_upgrade, major_ignore_test, auto_yes);
        } else {
            result = upgrade_single_package_v2(package_name, elm_json, env, major_upgrade, major_ignore_test, auto_yes);
        }
    } else {
        log_debug("Using V1 protocol for upgrade");
        if (upgrade_all) {
            result = upgrade_all_packages_v1(elm_json, env, major_upgrade, major_ignore_test, auto_yes);
        } else {
            result = upgrade_single_package_v1(package_name, elm_json, env, major_upgrade, major_ignore_test, auto_yes);
        }
    }

    elm_json_free(elm_json);
    install_env_free(env);

    log_set_level(original_level);

    return result;
}
