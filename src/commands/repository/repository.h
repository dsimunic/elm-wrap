#ifndef REPOSITORY_H
#define REPOSITORY_H

/**
 * Repository command group - manage local package repositories
 * 
 * Commands:
 *   repository new [<root_path>] [--compiler <name>] [--version <version>]
 *   repository list [<root_path>]
 */

/* Main entry point for the 'repository' command group */
int cmd_repository(int argc, char *argv[]);

/* Subcommand: new - create a new repository directory */
int cmd_repository_new(int argc, char *argv[]);

/* Subcommand: list - list repositories at path */
int cmd_repository_list(int argc, char *argv[]);

#endif /* REPOSITORY_H */
