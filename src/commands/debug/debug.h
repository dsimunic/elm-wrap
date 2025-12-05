#ifndef DEBUG_H
#define DEBUG_H

/**
 * Debug command group - diagnostic tools for development
 */

/* Main entry point for the 'debug' command group */
int cmd_debug(int argc, char *argv[]);

/* Subcommand: include-tree - show include/import dependency tree */
int cmd_debug_include_tree(int argc, char *argv[]);

/* Subcommand: install-plan - show what packages would be installed (dry-run) */
int cmd_debug_install_plan(int argc, char *argv[]);

/* Subcommand: registry_v1 - manage V1 protocol registry.dat file */
int cmd_debug_registry_v1(int argc, char *argv[]);

#endif /* DEBUG_H */
