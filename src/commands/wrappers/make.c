#include "make.h"
#include "../../elm_json.h"
#include "../../global_context.h"
#include "elm_cmd_common.h"
#include "../../install_env.h"
#include "../../elm_compiler.h"
#include "../../alloc.h"
#include "../../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

#define ELM_JSON_PATH "elm.json"

static void print_make_usage(void) {
    printf("Usage: %s make ELM_FILE [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Compile Elm code to JavaScript or HTML.\n");
    printf("\n");
    printf("This command ensures all package dependencies are downloaded and cached\n");
    printf("before calling 'elm make' to perform the actual compilation.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s make src/Main.elm                 # Compile Main.elm\n", global_context_program_name());
    printf("  %s make src/Main.elm --output=main.js\n", global_context_program_name());
    printf("  %s make src/Main.elm --optimize\n", global_context_program_name());
    printf("\n");
    printf("All options are passed through to 'elm make'.\n");
}

int cmd_make(int argc, char *argv[]) {
    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_make_usage();
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
        log_error("Have you run 'elm init' or 'wrap init'?");
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

    // Now call elm make with all the arguments
    const char *compiler_name = global_context_compiler_name();
    printf("\nAll dependencies cached. Running %s make...\n\n", compiler_name);

    // Get elm compiler path
    char *elm_path = elm_compiler_get_path();
    if (!elm_path) {
        log_error("Could not find %s binary", compiler_name);
        log_error("Please install %s or set the WRAP_ELM_COMPILER_PATH environment variable", compiler_name);
        return 1;
    }

    log_debug("Using %s compiler at: %s", compiler_name, elm_path);

    // Build environment with https_proxy for offline mode
    char **elm_env = build_elm_environment();
    if (!elm_env) {
        log_error("Failed to build environment for %s", compiler_name);
        return 1;
    }

    // Build elm make command
    // We need to pass all arguments except "make" to elm
    char **elm_args = arena_malloc(sizeof(char*) * (argc + 2));
    elm_args[0] = (char*)compiler_name;
    elm_args[1] = "make";

    // Copy remaining arguments
    for (int i = 1; i < argc; i++) {
        elm_args[i + 1] = argv[i];
    }
    elm_args[argc + 1] = NULL;

    // Execute elm make with custom environment
    execve(elm_path, elm_args, elm_env);

    // If execve returns, it failed
    log_error("Failed to execute %s compiler at: %s", compiler_name, elm_path);
    if (getenv("WRAP_ELM_COMPILER_PATH")) {
        log_error("The compiler was not found at the path specified in WRAP_ELM_COMPILER_PATH");
    }
    perror("execve");
    arena_free(elm_args);
    return 1;
}
