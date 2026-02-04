#ifndef MIRROR_CMD_H
#define MIRROR_CMD_H

/*
 * Mirror command: Create a content-addressable mirror of Elm packages.
 *
 * Usage: wrap repository mirror [OPTIONS] [OUTPUT_DIR]
 *
 * Creates a mirror suitable for self-hosted infrastructure:
 * - Archives stored by SHA1 hash for deduplication
 * - elm.json and docs.json metadata in packages/ directory
 * - manifest.json mapping packages to hashes
 * - Incremental sync using sequence numbers
 */

int cmd_mirror(int argc, char *argv[]);

#endif /* MIRROR_CMD_H */
