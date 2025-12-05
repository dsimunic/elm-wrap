/**
 * info.c - Application info command implementation
 *
 * Displays application information (same as `package info` for applications).
 */

#include "application.h"
#include "../../install.h"
#include "../../progname.h"
#include <stdio.h>
#include <string.h>

static void print_application_info_usage(void) {
    printf("Usage: %s application info [PATH]\n", program_name);
    printf("\n");
    printf("Display application information.\n");
    printf("\n");
    printf("Shows:\n");
    printf("  - Current ELM_HOME directory\n");
    printf("  - Registry cache statistics\n");
    printf("  - Package registry connectivity\n");
    printf("  - Installed packages\n");
    printf("  - Available updates\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s application info              # Show info for current directory\n", program_name);
    printf("  %s application info ./path/to    # Show info for elm.json at path\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help   Show this help message\n");
}

int cmd_application_info(int argc, char *argv[]) {
    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_application_info_usage();
            return 0;
        }
    }

    // Delegate to the same code path as `package info`
    return cmd_info(argc, argv);
}
