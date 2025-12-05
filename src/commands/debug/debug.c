#include "debug.h"
#include "../../alloc.h"
#include "../../progname.h"
#include <stdio.h>
#include <string.h>

static void print_debug_usage(void) {
    printf("Usage: %s debug SUBCOMMAND [OPTIONS]\n", program_name);
    printf("\n");
    printf("Diagnostic tools for development.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  include-tree <path>  Show import dependency tree for a file or package\n");
    printf("  install-plan <pkg>   Show what packages would be installed for a package (dry-run)\n");
    printf("  registry_v1 <cmd>    Manage V1 protocol registry.dat file\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help           Show this help message\n");
}

int cmd_debug(int argc, char *argv[]) {
    if (argc < 2) {
        print_debug_usage();
        return 1;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "-h") == 0 || strcmp(subcmd, "--help") == 0) {
        print_debug_usage();
        return 0;
    }

    if (strcmp(subcmd, "include-tree") == 0) {
        return cmd_debug_include_tree(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "install-plan") == 0) {
        return cmd_debug_install_plan(argc - 1, argv + 1);
    }

    if (strcmp(subcmd, "registry_v1") == 0) {
        return cmd_debug_registry_v1(argc - 1, argv + 1);
    }

    fprintf(stderr, "Error: Unknown debug subcommand '%s'\n", subcmd);
    fprintf(stderr, "Run '%s debug --help' for usage information.\n", program_name);
    return 1;
}
