#ifndef KIT_H
#define KIT_H

/**
 * Kit command group - install a set of tools and packages from a kitfile.
 *
 * Commands:
 *   kit install PATH
 *   kit update --dry-run <KIT-URL|PATH>
 *
 * PATH may be a `.kit` manifest file or a directory containing a single
 * `.kit` manifest. See doc/tools/Kit.md for the full specification.
 */

/* Main entry point for the 'kit' command group */
int cmd_kit(int argc, char *argv[]);

/* Subcommand: install - install a kit from PATH */
int cmd_kit_install(int argc, char *argv[]);

/* Subcommand: update - non-destructively verify a kit from a URL or path.
 * Currently supports only --dry-run: it fetches/reads and parses the kit,
 * reports the tools and packages it would register/download, and writes
 * nothing to disk or the registry. */
int cmd_kit_update(int argc, char *argv[]);

#endif /* KIT_H */
