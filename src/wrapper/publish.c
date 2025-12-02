/**
 * publish.c - Deprecated publish wrapper command
 *
 * This command previously wrapped 'elm publish' but has been replaced by
 * 'package publish' which uses the new publishing flow with rulr rules.
 */

#include "publish.h"
#include "../progname.h"
#include <stdio.h>

int cmd_publish(int argc, char *argv[]) {
    (void)argc;  // Unused
    (void)argv;  // Unused

    fprintf(stderr, "%s uses a new publishing flow.\n", program_name);
    fprintf(stderr, "Please use %s package publish\n", program_name);
    return 100;
}
