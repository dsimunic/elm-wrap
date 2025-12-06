/**
 * init.c - Application init command implementation
 *
 * Core logic for initializing an Elm application project.
 * This is shared between `elm-wrap init` and `elm-wrap application init`.
 */

#include "application.h"
#include "../../install_env.h"
#include "../../global_context.h"
#include "../../elm_json.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../wrappers/init_v1.h"
#include "../wrappers/init_v2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

#define ELM_JSON_PATH "elm.json"
#define ANSI_DULL_CYAN "\033[36m"
#define ANSI_RESET "\033[0m"

static void print_application_init_usage(void) {
    printf("Usage: %s application init [options]\n", global_context_program_name());
    printf("\n");
    printf("Initialize a new Elm application project by creating an elm.json file.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes    Skip confirmation prompt and create elm.json immediately\n");
    printf("  -h, --help   Show this help message\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s application init       # Create a new elm.json with prompt\n", global_context_program_name());
    printf("  %s application init -y    # Create a new elm.json without prompt\n", global_context_program_name());
}

static bool elm_json_exists(void) {
    struct stat st;
    return stat(ELM_JSON_PATH, &st) == 0;
}

static void print_existing_project_error(void) {
    fprintf(stderr, "%s-- EXISTING PROJECT ------------------------------------------------------------\n", ANSI_DULL_CYAN);
    fprintf(stderr, "\n");
    fprintf(stderr, "You already have an elm.json file, so there is nothing for me to initialize!\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Maybe <%shttps://elm-lang.org/0.19.1/init%s> can help you figure out what to do\n", ANSI_DULL_CYAN, ANSI_RESET);
    fprintf(stderr, "next?\n");
    fprintf(stderr, "\n%s", ANSI_RESET);
}

static bool prompt_user_yes_no(void) {
    printf("Hello! Elm projects always start with an elm.json file. I can create them!\n");
    printf("\n");
    printf("Now you may be wondering, what will be in this file? How do I add Elm files to\n");
    printf("my project? How do I see it in the browser? How will my code grow? Do I need\n");
    printf("more directories? What about tests? Etc.\n");
    printf("\n");
    printf("Check out %s<https://elm-lang.org/0.19.1/init>%s for all the answers!\n", ANSI_DULL_CYAN, ANSI_RESET);
    printf("\n");
    printf("Knowing all that, would you like me to create an elm.json file now? [Y/n]: ");
    fflush(stdout);

    char response[256];
    if (!fgets(response, sizeof(response), stdin)) {
        return false;
    }

    // Trim whitespace
    char *start = response;
    while (isspace((unsigned char)*start)) start++;

    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    // Empty input or 'y' or 'Y' means yes
    if (*start == '\0' || *start == 'y' || *start == 'Y') {
        return true;
    }

    return false;
}

static bool create_src_directory(void) {
    struct stat st;
    if (stat("src", &st) == 0) {
        // Directory already exists
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        // Path exists but is not a directory
        log_error("'src' exists but is not a directory");
        return false;
    }

    // Create directory
    if (mkdir("src", 0755) != 0) {
        log_error("Failed to create 'src' directory");
        return false;
    }

    return true;
}

static bool solve_init_dependencies(InstallEnv *env, PackageMap **direct_deps, PackageMap **indirect_deps) {
    /* Dispatch to protocol-specific implementation */
    bool is_v2 = (env && env->protocol_mode == PROTOCOL_V2);

    log_debug("Using %s protocol for dependency resolution", is_v2 ? "V2" : "V1");

    if (is_v2) {
        return solve_init_dependencies_v2(env, direct_deps, indirect_deps);
    } else {
        return solve_init_dependencies_v1(env, direct_deps, indirect_deps);
    }
}

int cmd_application_init(int argc, char *argv[]) {
    bool skip_prompt = false;

    // Check for flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_application_init_usage();
            return 0;
        } else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            skip_prompt = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_application_init_usage();
            return 1;
        }
    }

    // Check if elm.json already exists
    if (elm_json_exists()) {
        print_existing_project_error();
        return 1;
    }

    // Prompt user (unless -y flag was provided)
    if (!skip_prompt) {
        if (!prompt_user_yes_no()) {
            printf("\nOkay, I did not make any changes!\n");
            return 0;
        }
    }

    // Initialize environment to ensure registry is available
    log_debug("Initializing environment for elm init");
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

    log_debug("Registry ready at: %s", env->cache->registry_path);

    // Solve for dependencies
    PackageMap *direct_deps = NULL;
    PackageMap *indirect_deps = NULL;

    if (!solve_init_dependencies(env, &direct_deps, &indirect_deps)) {
        log_error("Failed to solve dependencies");
        install_env_free(env);
        return 1;
    }

    // Create elm.json structure
    ElmJson *elm_json = arena_malloc(sizeof(ElmJson));
    if (!elm_json) {
        log_error("Failed to allocate elm.json structure");
        package_map_free(direct_deps);
        package_map_free(indirect_deps);
        install_env_free(env);
        return 1;
    }

    elm_json->type = ELM_PROJECT_APPLICATION;
    elm_json->elm_version = arena_strdup("0.19.1");
    elm_json->dependencies_direct = direct_deps;
    elm_json->dependencies_indirect = indirect_deps;
    elm_json->dependencies_test_direct = package_map_create();
    elm_json->dependencies_test_indirect = package_map_create();
    elm_json->package_name = NULL;
    elm_json->package_version = NULL;
    elm_json->package_dependencies = NULL;
    elm_json->package_test_dependencies = NULL;

    // Write elm.json file
    if (!elm_json_write(elm_json, ELM_JSON_PATH)) {
        log_error("Failed to write elm.json");
        elm_json_free(elm_json);
        install_env_free(env);
        return 1;
    }

    // Create src directory
    if (!create_src_directory()) {
        log_error("Failed to create src directory");
        elm_json_free(elm_json);
        install_env_free(env);
        return 1;
    }

    printf("\nOkay, I created it. Now read that link!\n");

    // Cleanup
    elm_json_free(elm_json);
    install_env_free(env);

    return 0;
}
