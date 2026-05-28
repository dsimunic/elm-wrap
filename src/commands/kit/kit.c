/**
 * kit.c - Kit command group dispatch
 *
 * Routes subcommands of the `kit` command group. See doc/tools/Kit.md.
 */

#include "kit.h"
#include "../../global_context.h"
#include <stdio.h>
#include <string.h>

static void print_kit_usage(void) {
    printf("Usage: %s kit SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Install a set of tools and packages from a kitfile manifest.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  install PATH          Install the kit at PATH (a .kit file or a directory)\n");
    printf("  update --dry-run SRC  Verify a kit (URL, .kit file, or directory) is consumable\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message\n");
}

int cmd_kit(int argc, char *argv[]) {
    if (argc < 2) {
        print_kit_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_kit_usage();
        return 0;
    }

    if (strcmp(subcmd, "install") == 0) {
        return cmd_kit_install(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "update") == 0) {
        return cmd_kit_update(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown kit subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s kit --help' for usage information.\n", global_context_program_name());
    return 1;
}
