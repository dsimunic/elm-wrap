/**
 * build.h - Build command header
 *
 * The build command generates a JSON build plan for Elm compilation.
 *
 * Subcommands:
 *   check - Display human-readable build plan and confirm before building
 */

#ifndef WRAPPER_BUILD_H
#define WRAPPER_BUILD_H

/**
 * Execute the build command.
 *
 * Usage: wrap build [SUBCOMMAND] [OPTIONS] PATH [PATH...]
 *
 * Subcommands:
 *   check    - Display human-readable build plan and confirm before building
 *   (none)   - Output JSON build plan (default)
 *
 * @param argc Argument count
 * @param argv Argument values
 * @return 0 on success, non-zero on failure
 */
int cmd_build(int argc, char *argv[]);

/**
 * Execute the build check subcommand.
 *
 * Analyzes the project, displays a human-readable build plan summary,
 * and prompts the user to proceed with compilation.
 *
 * Usage: wrap build check [OPTIONS] PATH [PATH...]
 *
 * Options:
 *   -y, --yes   Skip confirmation and proceed with build
 *   -n, --no    Show plan only, do not prompt or build
 *
 * @param argc Argument count
 * @param argv Argument values
 * @return 0 on success, non-zero on failure
 */
int cmd_build_check(int argc, char *argv[]);

#endif /* WRAPPER_BUILD_H */
