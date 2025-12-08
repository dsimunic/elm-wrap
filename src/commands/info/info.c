/**
 * info.c - Top-level info command alias.
 *
 * Provides an alias for displaying package or application information.
 */

#include "../../alloc.h"
#include "info.h"
#include "../../install.h"

int cmd_info_command(int argc, char *argv[]) {
    return cmd_info(argc, argv, "info");
}
