/**
 * init.c - Wrapper init command
 *
 * Delegates to the application init command implementation.
 */

#include "init.h"
#include "../application/application.h"

int cmd_init(int argc, char *argv[]) {
    /* Delegate to application init command */
    return cmd_application_init(argc, argv);
}
