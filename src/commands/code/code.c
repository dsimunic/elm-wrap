#include "code.h"
#include "../../alloc.h"
#include "../../global_context.h"
#include <stdio.h>
#include <string.h>

static void print_code_usage(void) {
    printf("Usage: %s code SUBCOMMAND [OPTIONS]\n", global_context_program_name());
    printf("\n");
    printf("Code analysis and transformation commands.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  format FILE        Parse and canonicalize Elm source, output AST\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
}

int cmd_code(int argc, char *argv[]) {
    if (argc < 2) {
        print_code_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_code_usage();
        return 0;
    }

    if (strcmp(subcmd, "format") == 0) {
        return cmd_code_format(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown code subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s code --help' for usage information.\n", global_context_program_name());
    return 1;
}
