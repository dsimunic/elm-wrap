#ifndef EXTRACT_CMD_H
#define EXTRACT_CMD_H

/**
 * Extract Elm source from an application into a new package.
 *
 * Creates a new package at TARGET_PATH, moves selected sources,
 * exposes modules, and adds the new package as a local-dev dependency.
 *
 * @param argc Argument count
 * @param argv Argument vector (argv[0] is "extract")
 * @return 0 on success, non-zero on failure
 */
int cmd_extract(int argc, char *argv[]);

#endif /* EXTRACT_CMD_H */
