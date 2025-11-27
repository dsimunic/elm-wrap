#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <libgen.h>
#include "buildinfo.h"
#include "install.h"
#include "make.h"
#include "init.h"
#include "repl.h"
#include "reactor.h"
#include "bump.h"
#include "diff.h"
#include "publish.h"
#include "config.h"
#include "commands/code/code.h"
#include "alloc.h"
#include "log.h"
#include "progname.h"

void print_usage(const char *prog) {
    printf("Usage: %s COMMAND [OPTIONS]\n", prog);
    printf("\nCommands:\n");
    printf("  repl               Open an interactive Elm REPL\n");
    printf("  init               Initialize a new Elm project\n");
    printf("  reactor            Start the Elm Reactor development server\n");
    printf("  make <ELM_FILE>    Compile Elm code to JavaScript or HTML\n");
    printf("  install <PACKAGE>  Install packages for your Elm project\n");
    printf("  bump               Bump version based on API changes\n");
    printf("  diff [VERSION]     Show API differences between versions\n");
    printf("\n");
    printf("  config             Display current configuration\n");
    printf("  publish SUBCOMMAND Publishing commands\n");
    printf("  package SUBCOMMAND Package management commands\n");
    printf("  code SUBCOMMAND    Code analysis and transformation commands\n");
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

    fprintf(stderr, "Error: Unknown package subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s package --help' for usage information.\n", prog);
    return 1;
}

int main(int argc, char *argv[]) {
    alloc_init();

    // Set global program name (extract basename from path)
    program_name = basename(argv[0]);

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

        if (strcmp(argv[1], "publish") == 0) {
            return cmd_publish(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "config") == 0) {
            return cmd_config(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "code") == 0) {
            return cmd_code(argc - 1, argv + 1);
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
