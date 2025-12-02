/**
 * live.c - Live command wrapper for Lamdera
 *
 * This command is only available when the compiler binary is 'lamdera'.
 * It wraps 'lamdera live' with dependency caching.
 */

#include "live.h"
#include "../elm_json.h"
#include "../elm_cmd_common.h"
#include "../install_env.h"
#include "../elm_compiler.h"
#include "../alloc.h"
#include "../log.h"
#include "../progname.h"
#include "../global_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

#define ELM_JSON_PATH "elm.json"

static void print_live_usage(void) {
    printf("Usage: %s live [OPTIONS]\n", program_name);
    printf("\n");
    printf("Start the Lamdera live development server.\n");
    printf("\n");
    printf("This command ensures all package dependencies are downloaded and cached\n");
    printf("before calling 'lamdera live'.\n");
    printf("\n");
    printf("All options are passed through to 'lamdera live'.\n");
}

int cmd_live(int argc, char *argv[]) {
    // Check if compiler is lamdera
    if (!global_context_is_lamdera()) {
        fprintf(stderr, "Error: The 'live' command is only available when using the Lamdera compiler.\n");
        fprintf(stderr, "Set ELM_WRAP_ELM_COMPILER_PATH to point to your lamdera binary.\n");
        return 1;
    }

    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_live_usage();
            return 0;
        }
    }

    // Initialize environment
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

    log_debug("ELM_HOME: %s", env->cache->elm_home);

    // Read elm.json
    log_debug("Reading elm.json");
    ElmJson *elm_json = elm_json_read(ELM_JSON_PATH);
    if (!elm_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'lamdera init' or 'wrap init'?");
        install_env_free(env);
        return 1;
    }

    // Download all packages
    int result = download_all_packages(elm_json, env);

    // Cleanup
    elm_json_free(elm_json);
    install_env_free(env);

    if (result != 0) {
        log_error("Failed to download all dependencies");
        return result;
    }

    // Now call lamdera live with all the arguments
    printf("\nAll dependencies cached. Running lamdera live...\n\n");

    // Get lamdera compiler path
    char *lamdera_path = elm_compiler_get_path();
    if (!lamdera_path) {
        log_error("Could not find lamdera binary");
        log_error("Please install lamdera or set the ELM_WRAP_ELM_COMPILER_PATH environment variable");
        return 1;
    }

    log_debug("Using lamdera compiler at: %s", lamdera_path);

    // Build environment with https_proxy for offline mode
    char **elm_env = build_elm_environment();
    if (!elm_env) {
        log_error("Failed to build environment for lamdera");
        return 1;
    }

    // Build lamdera live command
    char **elm_args = arena_malloc(sizeof(char*) * (argc + 2));
    elm_args[0] = "lamdera";
    elm_args[1] = "live";

    // Copy remaining arguments
    for (int i = 1; i < argc; i++) {
        elm_args[i + 1] = argv[i];
    }
    elm_args[argc + 1] = NULL;

    // Execute lamdera live with custom environment
    execve(lamdera_path, elm_args, elm_env);

    // If execve returns, it failed
    log_error("Failed to execute lamdera compiler at: %s", lamdera_path);
    if (getenv("ELM_WRAP_ELM_COMPILER_PATH")) {
        log_error("The compiler was not found at the path specified in ELM_WRAP_ELM_COMPILER_PATH");
    }
    perror("execve");
    arena_free(elm_args);
    return 1;
}
