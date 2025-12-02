#include "init.h"
#include "../install_env.h"
#include "../elm_json.h"
#include "../alloc.h"
#include "../log.h"
#include "../registry.h"
#include "../progname.h"
#include "../pgsolver/pg_core.h"
#include "../pgsolver/pg_elm.h"
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

static void print_init_usage(void) {
    printf("Usage: %s init [options]\n", program_name);
    printf("\n");
    printf("Initialize a new Elm project by creating an elm.json file.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes    Skip confirmation prompt and create elm.json immediately\n");
    printf("  -h, --help   Show this help message\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s init       # Create a new elm.json with prompt\n", program_name);
    printf("  %s init -y    # Create a new elm.json without prompt\n", program_name);
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

static const char *pg_name_resolver(void *ctx, PgPackageId pkg) {
    PgElmContext *pg_ctx = (PgElmContext *)ctx;
    if (!pg_ctx || pkg < 0 || pkg >= pg_ctx->package_count) {
        return "?";
    }
    if (pkg == pg_elm_root_package_id()) {
        return "root";
    }
    // Return "author/name" format
    static char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s/%s", pg_ctx->authors[pkg], pg_ctx->names[pkg]);
    return buffer;
}

static bool solve_init_dependencies(InstallEnv *env, PackageMap **direct_deps, PackageMap **indirect_deps) {
    // Create PubGrub solver context
    PgElmContext *pg_ctx = pg_elm_context_new(env, true);
    if (!pg_ctx) {
        log_error("Failed to create PubGrub solver context");
        return false;
    }

    // Add the three required packages as root dependencies with "any" version constraint
    const char *required_packages[][2] = {
        {"elm", "browser"},
        {"elm", "core"},
        {"elm", "html"}
    };

    for (int i = 0; i < 3; i++) {
        const char *author = required_packages[i][0];
        const char *name = required_packages[i][1];

        PgPackageId pkg_id = pg_elm_intern_package(pg_ctx, author, name);
        if (pkg_id < 0) {
            log_error("Failed to intern package %s/%s", author, name);
            pg_elm_context_free(pg_ctx);
            return false;
        }

        // Use "any" version constraint to get the latest
        PgVersionRange range = pg_range_any();
        if (!pg_elm_add_root_dependency(pg_ctx, pkg_id, range)) {
            log_error("Failed to add root dependency for %s/%s", author, name);
            pg_elm_context_free(pg_ctx);
            return false;
        }
    }

    // Create dependency provider
    PgDependencyProvider provider = pg_elm_make_provider(pg_ctx);
    PgPackageId root_pkg = pg_elm_root_package_id();

    PgVersion root_version;
    root_version.major = 1;
    root_version.minor = 0;
    root_version.patch = 0;

    // Create solver
    PgSolver *solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version);
    if (!solver) {
        log_error("Failed to create PubGrub solver");
        pg_elm_context_free(pg_ctx);
        return false;
    }

    // Run solver
    PgSolverStatus status = pg_solver_solve(solver);

    if (status != PG_SOLVER_OK) {
        log_error("Failed to solve dependencies");
        char error_msg[4096];
        if (pg_solver_explain_failure(solver, pg_name_resolver, pg_ctx, error_msg, sizeof(error_msg))) {
            fprintf(stderr, "%s\n", error_msg);
        }
        pg_solver_free(solver);
        pg_elm_context_free(pg_ctx);
        return false;
    }

    // Create package maps for direct and indirect dependencies
    *direct_deps = package_map_create();
    *indirect_deps = package_map_create();

    if (!*direct_deps || !*indirect_deps) {
        log_error("Failed to create package maps");
        package_map_free(*direct_deps);
        package_map_free(*indirect_deps);
        pg_solver_free(solver);
        pg_elm_context_free(pg_ctx);
        return false;
    }

    // Add the three required packages to direct dependencies
    for (int i = 0; i < 3; i++) {
        const char *author = required_packages[i][0];
        const char *name = required_packages[i][1];

        PgPackageId pkg_id = pg_elm_intern_package(pg_ctx, author, name);
        PgVersion version;
        if (!pg_solver_get_selected_version(solver, pkg_id, &version)) {
            log_error("Failed to get version for %s/%s", author, name);
            package_map_free(*direct_deps);
            package_map_free(*indirect_deps);
            pg_solver_free(solver);
            pg_elm_context_free(pg_ctx);
            return false;
        }

        char version_str[32];
        snprintf(version_str, sizeof(version_str), "%d.%d.%d", version.major, version.minor, version.patch);

        if (!package_map_add(*direct_deps, author, name, version_str)) {
            log_error("Failed to add %s/%s to direct dependencies", author, name);
            package_map_free(*direct_deps);
            package_map_free(*indirect_deps);
            pg_solver_free(solver);
            pg_elm_context_free(pg_ctx);
            return false;
        }
    }

    // Get all other resolved packages and add them to indirect dependencies
    for (int pkg_id = 1; pkg_id < pg_ctx->package_count; pkg_id++) {
        // Skip the three direct dependencies
        bool is_direct = false;
        for (int i = 0; i < 3; i++) {
            const char *author = required_packages[i][0];
            const char *name = required_packages[i][1];
            PgPackageId direct_pkg_id = pg_elm_intern_package(pg_ctx, author, name);
            if (pkg_id == direct_pkg_id) {
                is_direct = true;
                break;
            }
        }
        if (is_direct) {
            continue;
        }

        PgVersion version;
        if (!pg_solver_get_selected_version(solver, pkg_id, &version)) {
            // No decision for this package, skip it
            continue;
        }

        const char *author = pg_ctx->authors[pkg_id];
        const char *name = pg_ctx->names[pkg_id];

        char version_str[32];
        snprintf(version_str, sizeof(version_str), "%d.%d.%d", version.major, version.minor, version.patch);

        if (!package_map_add(*indirect_deps, author, name, version_str)) {
            log_error("Failed to add %s/%s to indirect dependencies", author, name);
            // Continue anyway
        }
    }

    pg_solver_free(solver);
    pg_elm_context_free(pg_ctx);
    return true;
}

int cmd_init(int argc, char *argv[]) {
    bool skip_prompt = false;

    // Check for flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_init_usage();
            return 0;
        } else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0) {
            skip_prompt = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_init_usage();
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
