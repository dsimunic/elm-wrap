#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include "buildinfo.h"
#include "install.h"
#include "commands/wrappers/make.h"
#include "commands/wrappers/init.h"
#include "commands/wrappers/repl.h"
#include "commands/wrappers/reactor.h"
#include "commands/wrappers/bump.h"
#include "commands/wrappers/diff.h"
#include "commands/wrappers/publish.h"
#include "commands/wrappers/live.h"
#include "config.h"
#include "commands/code/code.h"
#include "commands/debug/debug.h"
#include "commands/policy/policy.h"
#include "commands/review/review.h"
#include "commands/application/application.h"
#include "commands/publish/package/package_publish.h"
#include "commands/publish/docs/docs.h"
#include "commands/repository/repository.h"
#include "commands/info/info.h"
#include "alloc.h"
#include "log.h"
#include "features.h"
#include "rulr/builtin_rules.h"
#include "global_context.h"
#include "embedded_archive.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void print_usage(const char *prog) {
    CompilerType compiler_type = global_context_compiler_type();

    printf("Usage: %s COMMAND [OPTIONS]\n", prog);
    printf("\nCommands:\n");

    /*
     * Compiler command sets:
     * 
     * elm:     repl, init, reactor, make, install, bump, diff, publish
     * lamdera: live, login, check, deploy, init, repl, reset, update,
     *          annotate, eval
     * wrapc:   make
     */
    switch (compiler_type) {
        case COMPILER_WRAPC:
            /* wrapc only supports make */
            printf("  make ELM_FILE        Compile Elm code to JavaScript or HTML\n");
            break;

        case COMPILER_LAMDERA:
            /* Lamdera commands - ordered as in lamdera --help */
            printf("  live                 Local development with live reload\n");
            printf("  login                Log in to the Lamdera CLI\n");
            printf("  check                Compile and type-check against deployed app\n");
            printf("  deploy               Deploy Lamdera app after a successful check\n");
            printf("  init                 Start a Lamdera Elm project\n");
            printf("  install PACKAGE      Install packages for your Elm project\n");
            printf("  make ELM_FILE        Compile Elm code to JavaScript or HTML\n");
            printf("  repl                 Open an interactive programming session\n");
            printf("  reset                Delete all compiler caches\n");
            printf("  update               Update the Lamdera compiler to latest version\n");
            printf("  annotate FILE EXPR   Print the type annotation for expression\n");
            printf("  eval FILE EXPR       Evaluate an expression\n");
            break;

        case COMPILER_ELM:
        case COMPILER_UNKNOWN:
        default:
            /* Standard Elm commands */
            printf("  repl               Open an interactive Elm REPL\n");
            printf("  init               Initialize a new Elm project\n");
            printf("  reactor            Start the Elm Reactor development server\n");
            printf("  make ELM_FILE      Compile Elm code to JavaScript or HTML\n");
            printf("  install PACKAGE    Install packages for your Elm project\n");
//            printf("  bump               Bump version based on API changes\n");
//            printf("  diff [VERSION]     Show API differences between versions\n");
            break;
    }

    printf("\n");
    printf("  config                    Display current configuration\n");
    printf("  info [PATH | PACKAGE [VERSION]]  Display package or application info\n");
    printf("  application SUBCOMMAND    Application management commands\n");
    printf("  package SUBCOMMAND        Package management commands\n");
    printf("  repository SUBCOMMAND     Repository management commands\n");
    if (feature_code_enabled()) {
        printf("  code SUBCOMMAND           Code analysis and transformation commands\n");
    }
    if (feature_policy_enabled()) {
        printf("  policy SUBCOMMAND         View and manage rulr policy rules\n");
    }
    if (feature_review_enabled()) {
        printf("  review SUBCOMMAND         Run review rules against Elm files\n");
    }
    printf("  debug SUBCOMMAND          Diagnostic tools for development\n");
    printf("\nOptions:\n");
    printf("  -v, --verbose      Show detailed logging output\n");
    printf("  -vv                Show extra verbose (trace) logging output\n");
    printf("  -V                 Show version number\n");
    printf("  --version          Show detailed version information\n");
    printf("  --sbom, --spdx     Show Software Bill of Materials (SBOM)\n");
    printf("  -h, --help         Show this help message\n");
}

void print_package_usage(const char *prog) {
    printf("Usage: %s package SUBCOMMAND [OPTIONS]\n", prog);
    printf("\nSubcommands:\n");
    printf("  install PACKAGE                Add a dependency to current elm.json\n");
    printf("  init PACKAGE                   Initialize a package\n");
    printf("  upgrade PACKAGE                Upgrade packages to latest versions\n");
    printf("  remove | uninstall  PACKAGE    Remove a package from elm.json\n");
    printf("  info    [ PATH                 Display package information and upgrades\n");
    printf("          | PACKAGE [VERSION]\n");
    printf("          ]\n");
    if (feature_publish_enabled()) {
        printf("  publish PATH                   Show files that would be published from a package\n");
    }
    printf("  docs    PATH                   Generate documentation JSON for a package\n");
    if (feature_cache_enabled()) {
        printf("  cache   PACKAGE                Download package to ELM_HOME without adding it to elm.json\n");
    }
    printf("\nOptions:\n");
    printf("  -y, --yes            Automatically confirm changes\n");
    printf("  -v, --verbose        Show detailed logging output\n");
    printf("  -vv                  Show extra verbose (trace) logging output\n");
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

    if (strcmp(subcmd, "init") == 0) {
        return cmd_package_init(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "cache") == 0) {
        if (!feature_cache_enabled()) {
            log_error("Subcommand 'cache' is not available in this build.");
            return 1;
        }
        // Pass remaining args to cache command
        return cmd_cache(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "remove") == 0 || strcmp(subcmd, "uninstall") == 0) {
        // Pass remaining args to remove command
        return cmd_remove(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "upgrade") == 0) {
        // Pass remaining args to upgrade command
        return cmd_upgrade(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "info") == 0) {
        // Pass remaining args to info command
        return cmd_info(argc - 1, argv + 1, "package info");
    }

    if (strcmp(subcmd, "publish") == 0) {
        if (!feature_publish_enabled()) {
            log_error("Subcommand 'publish' is not available in this build.");
            return 1;
        }
        // Pass remaining args to package publish command
        return cmd_package_publish(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "docs") == 0) {
        // Pass remaining args to docs command
        return cmd_publish_docs(argc - 1, argv + 1);
    }

    log_error("Unknown package subcommand '%s'", subcmd);
    log_error("Run '%s package --help' for usage information.", prog);
    return 1;
}

int main(int argc, char *argv[]) {
    alloc_init();

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

        if (!got_exe_path) {
            char *resolved = realpath(argv[0], NULL);
            if (resolved) {
                strncpy(exe_path, resolved, sizeof(exe_path) - 1);
                exe_path[sizeof(exe_path) - 1] = '\0';
                free(resolved);
                got_exe_path = true;
            }
        }
        
        if (got_exe_path) {
            embedded_archive_init(exe_path);
            builtin_rules_init(exe_path);
        }
    }

    // Parse global flags
    int verbosity = 0;
    int cmd_start = 1;

    // Check for global verbose flag before command
    // -v enables debug, -vv (or -v -v) enables trace
    for (int i = 1; i < argc && cmd_start == 1; i++) {
        if (strcmp(argv[i], "-vv") == 0) {
            verbosity += 2;
            // Shift remaining args
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--; // Check same position again
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbosity++;
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
    log_init(verbosity);

    // Initialize global context (determines V1 vs V2 mode and stores program name from argv[0])
    global_context_init(argv[0]);

    if (argc > 1) {
        if (strcmp(argv[1], "-V") == 0) {
            printf("%s\n", build_base_version);
            return 0;
        }

        if (strcmp(argv[1], "--version") == 0) {
            print_version_info();
            return 0;
        }

        if (strcmp(argv[1], "--sbom") == 0 || strcmp(argv[1], "--spdx") == 0) {
            print_sbom_full();
            return 0;
        }

        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(global_context_program_name());
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

        if (strcmp(argv[1], "info") == 0) {
            return cmd_info_command(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "package") == 0) {
            return cmd_package(argc - 1, argv + 1, global_context_program_name());
        }

        if (strcmp(argv[1], "application") == 0 || strcmp(argv[1], "app") == 0) {
            return cmd_application(argc - 1, argv + 1);
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
            if (!feature_publish_enabled()) {
                log_error("Command 'publish' is not available in this build.");
                return 1;
            }
            return cmd_publish(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "config") == 0) {
            return cmd_config(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "code") == 0) {
            if (!feature_code_enabled()) {
                log_error("Command 'code' is not available in this build.");
                return 1;
            }
            return cmd_code(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "policy") == 0) {
            if (!feature_policy_enabled()) {
                log_error("Command 'policy' is not available in this build.");
                return 1;
            }
            return cmd_policy(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "review") == 0) {
            if (!feature_review_enabled()) {
                log_error("Command 'review' is not available in this build.");
                return 1;
            }
            return cmd_review(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "repository") == 0) {
            return cmd_repository(argc - 1, argv + 1);
        }

        if (strcmp(argv[1], "debug") == 0) {
            return cmd_debug(argc - 1, argv + 1);
        }

        // Unknown command
        log_error("Unknown command '%s'", argv[1]);
        log_error("Run '%s --help' for usage information.", global_context_program_name());
        return 1;
    }

    // No command specified
    print_usage(global_context_program_name());
    return 1;
}
