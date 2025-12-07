/**
 * application.c - Application command group
 *
 * Provides commands for managing Elm application projects.
 */

#include "application.h"
#include "../../alloc.h"
#include "../../global_context.h"
#include <stdio.h>
#include <string.h>

static void print_application_usage(void) {
    printf("Usage: %s application SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Application management commands.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  init [TEMPLATE]    Initialize a new Elm application (default: application)\n");
    printf("  info [PATH]        Display application information and upgrades\n");
    printf("  list-templates     List available application templates\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
}

int cmd_application(int argc, char *argv[]) {
    if (argc < 2) {
        print_application_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_application_usage();
        return 0;
    }

    if (strcmp(subcmd, "init") == 0) {
        return cmd_application_init(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "info") == 0) {
        return cmd_application_info(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "list-templates") == 0) {
        return cmd_application_list_templates(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown application subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s application --help' for usage information.\n", global_context_program_name());
    return 1;
}
