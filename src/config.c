#include "config.h"
#include "cache.h"
#include "elm_compiler.h"
#include "global_context.h"
#include "alloc.h"
#include "log.h"
#include "env_defaults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_config_usage(void) {
    printf("Usage: %s config\n", global_context_program_name());
    printf("\n");
    printf("Display current configuration.\n");
    printf("\n");
    printf("Shows:\n");
    printf("  - Protocol mode (V1 or V2)\n");
    printf("  - ELM_HOME directory\n");
    printf("  - Elm compiler version\n");
    printf("  - Elm compiler binary path\n");
}

int cmd_config(int argc, char *argv[]) {
    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_config_usage();
            return 0;
        }
    }

    // Get cache configuration to retrieve ELM_HOME
    CacheConfig *cache = cache_config_init();
    if (!cache) {
        log_error("Failed to initialize cache configuration");
        return 1;
    }

    // Get compiler path
    char *compiler_path = elm_compiler_get_path();
    if (!compiler_path) {
        printf("ELM_HOME: %s\n", cache->elm_home);
        printf("Elm compiler version: (not found)\n");
        printf("Elm compiler path: (not found)\n");
        cache_config_free(cache);
        return 0;
    }

    // Get compiler version
    char *compiler_version = elm_compiler_get_version();

    // Get global context for protocol mode
    GlobalContext *ctx = global_context_get();

    // Print configuration
    printf("Protocol mode: %s\n", global_context_mode_string());
    if (ctx && ctx->protocol_mode == PROTOCOL_V2 && ctx->repository_path) {
        printf("Repository path: %s\n", ctx->repository_path);
    }
    printf("ELM_HOME: %s\n", cache->elm_home);
    if (env_get_offline_mode()) {
        printf("Offline mode: forced (WRAP_OFFLINE_MODE=1)\n");
    } else {
        printf("Offline mode: auto-detect\n");
    }
    if (compiler_version) {
        printf("Elm compiler version: %s\n", compiler_version);
    } else {
        printf("Elm compiler version: (could not determine)\n");
    }
    printf("Elm compiler path: %s\n", compiler_path);

    // Cleanup
    cache_config_free(cache);

    return 0;
}
