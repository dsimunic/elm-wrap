#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include "buildinfo.h"
#include "install.h"
#include "wrapper/make.h"
#include "wrapper/init.h"
#include "wrapper/repl.h"
#include "wrapper/reactor.h"
#include "wrapper/bump.h"
#include "wrapper/diff.h"
#include "wrapper/publish.h"
#include "wrapper/live.h"
#include "config.h"
#include "commands/code/code.h"
#include "commands/debug/debug.h"
#include "commands/policy/policy.h"
#include "commands/review/review.h"
#include "commands/publish/package/package_publish.h"
#include "commands/publish/docs/docs.h"
#include "commands/repository/repository.h"
#include "alloc.h"
#include "log.h"
#include "progname.h"
#include "rulr/builtin_rules.h"
#include "global_context.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void print_usage(const char *prog) {
    GlobalContext *ctx = global_context_get();
    bool is_lamdera = ctx && ctx->compiler_name && strcmp(ctx->compiler_name, "lamdera") == 0;

    printf("Usage: %s COMMAND [OPTIONS]\n", prog);
    printf("\nCommands:\n");
    printf("  repl               Open an interactive Elm REPL\n");
    printf("  init               Initialize a new Elm project\n");
    printf("  reactor            Start the Elm Reactor development server\n");
    printf("  make <ELM_FILE>    Compile Elm code to JavaScript or HTML\n");
    printf("  install <PACKAGE>  Install packages for your Elm project\n");
    printf("  bump               Bump version based on API changes\n");
    printf("  diff [VERSION]     Show API differences between versions\n");
    if (is_lamdera) {
        printf("  live               Start the Lamdera live development server\n");
    }
    printf("\n");
    printf("  config             Display current configuration\n");
    printf("  package SUBCOMMAND Package management commands\n");
    printf("  repository SUBCOMMAND Repository management commands\n");
    printf("  code SUBCOMMAND    Code analysis and transformation commands\n");
    printf("  policy SUBCOMMAND  View and manage rulr policy rules\n");
    printf("  review SUBCOMMAND  Run rulr rules against Elm files\n");
    printf("  debug SUBCOMMAND   Diagnostic tools for development\n");
    printf("\nOptions:\n");
    printf("  -v, --verbose      Show detailed logging output\n");
    printf("  -V                 Show version number\n");
    printf("  --version          Show detailed version information\n");
    printf("  -h, --help         Show this help message\n");
}

void print_package_usage(const char *prog) {
    printf("Usage: %s package SUBCOMMAND [OPTIONS]\n", prog);
    printf("\nSubcommands:\n");
    printf("  install [<PACKAGE>]  Install packages for your Elm project\n");
    printf("  cache [<PACKAGE>]    Download packages to cache without prompting\n");
    printf("  remove <PACKAGE>     Remove a package from your Elm project\n");
    printf("  upgrade [PACKAGE]    Upgrade packages to latest versions\n");
    printf("  check [elm.json]     Check for available package upgrades\n");
    printf("  info                 Display package management information\n");
    printf("  deps <PACKAGE>       Display dependencies for a specific package\n");
    printf("  publish <PATH>       Show files that would be published from a package\n");
    printf("  docs <PATH>          Generate documentation JSON for a package\n");
    printf("\nOptions:\n");
    printf("  -y, --yes            Automatically confirm changes\n");
    printf("  -v, --verbose        Show detailed logging output\n");
    printf("  -h, --help           Show this help message\n");
}

int cmd_package(int argc, char *argv[], const char *prog) {
    if (argc < 2) {
        print_package_usage(prog);
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_package_usage(prog);
        return 0;
    }

    if (strcmp(subcmd, "install") == 0) {
        // Pass remaining args to install command
        return cmd_install(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "cache") == 0) {
        // Pass remaining args to cache command
        return cmd_cache(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "remove") == 0) {
        // Pass remaining args to remove command
        return cmd_remove(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "upgrade") == 0) {
        // Pass remaining args to upgrade command
        return cmd_upgrade(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "check") == 0) {
        // Pass remaining args to check command
        return cmd_check(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "info") == 0) {
        // Pass remaining args to info command
        return cmd_info(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "deps") == 0) {
        // Pass remaining args to deps command
        return cmd_deps(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "publish") == 0) {
        // Pass remaining args to package publish command
        return cmd_package_publish(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "docs") == 0) {
        // Pass remaining args to docs command
        return cmd_publish_docs(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown package subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s package --help' for usage information.\n", prog);
    return 1;
}

int main(int argc, char *argv[]) {
    alloc_init();

    // Set global program name (extract basename from path)
    program_name = basename(argv[0]);

    /* 
     * Initialize built-in rules subsystem.
     * Get the full path to the executable to find the embedded zip archive.
     */
    {
        char exe_path[PATH_MAX];
        bool got_exe_path = false;
        
#ifdef __APPLE__
        uint32_t size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &size) == 0) {
            char *resolved = realpath(exe_path, NULL);
            if (resolved) {
                strncpy(exe_path, resolved, sizeof(exe_path) - 1);
                exe_path[sizeof(exe_path) - 1] = '\0';
                free(resolved);
                got_exe_path = true;
            }
        }
#elif defined(__linux__)
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len > 0) {
            exe_path[len] = '\0';
            got_exe_path = true;
        }
#endif
        
        if (got_exe_path) {
            builtin_rules_init(exe_path);
        }
    }

    // Parse global flags
    bool verbose = false;
    int cmd_start = 1;

    // Check for global verbose flag before command
    for (int i = 1; i < argc && cmd_start == 1; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
            // Shift remaining args
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--; // Check same position again
        } else if (argv[i][0] != '-') {
            // Found command
            break;
        }
    }

    // Initialize logging
    log_init(verbose);

    // Initialize global context (determines V1 vs V2 mode)
    global_context_init();

    if (argc > 1) {
        if (strcmp(argv[1], "-V") == 0) {
            printf("%s\n", build_base_version);
            return 0;
        }

        if (strcmp(argv[1], "--version") == 0) {
            print_version_info();
            return 0;
        }

        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(program_name);
            return 0;
        }

        // Command routing
        if (strcmp(argv[1], "init") == 0) {
            return cmd_init(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "make") == 0) {
            return cmd_make(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "repl") == 0) {
            return cmd_repl(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "reactor") == 0) {
            return cmd_reactor(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "install") == 0) {
            return cmd_install(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "package") == 0) {
            return cmd_package(argc - 1, argv + 1, program_name);
        }

        if (strcmp(argv[1], "bump") == 0) {
            return cmd_bump(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "diff") == 0) {
            return cmd_diff(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "live") == 0) {
            return cmd_live(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "publish") == 0) {
            return cmd_publish(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "config") == 0) {
            return cmd_config(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "code") == 0) {
            return cmd_code(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "policy") == 0) {
            return cmd_policy(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "review") == 0) {
            return cmd_review(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "repository") == 0) {
            return cmd_repository(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "debug") == 0) {
            return cmd_debug(argc - 1, argv + 1);
        }

        // Unknown command
        fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
        fprintf(stderr, "Run '%s --help' for usage information.\n", program_name);
        return 1;
    }

    // No command specified
    print_usage(program_name);
    return 1;
}
