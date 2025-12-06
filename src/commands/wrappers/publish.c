/**
 * publish.c - Deprecated publish wrapper command
 *
 * This command previously wrapped 'elm publish' but has been replaced by
 * 'package publish' which uses the new publishing flow with rulr rules.
 */

#include "publish.h"
#include "../../global_context.h"
#include <stdio.h>

int cmd_publish(int argc, char *argv[]) {
    (void)argc;  // Unused
    (void)argv;  // Unused

    fprintf(stderr, "%s uses a new publishing flow.\n", global_context_program_name());
    fprintf(stderr, "Please use %s package publish\n", global_context_program_name());
    return 100;
}
