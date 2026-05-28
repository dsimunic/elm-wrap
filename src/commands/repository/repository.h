#ifndef REPOSITORY_H
#define REPOSITORY_H

/**
 * Repository command group - manage local package repositories
 *
 * Commands:
 *   repository new PATH [--compiler COMPILER [--version VERSION]]
 *   repository list [PATH]
 *   repository local-dev [clear ...]
 *   repository mirror [OPTIONS] [OUTPUT_DIR]
 */

/* Main entry point for the 'repository' command group */
int cmd_repository(int argc, char *argv[]);

/* Subcommand: new - create a new repository directory */
int cmd_repository_new(int argc, char *argv[]);

/* Subcommand: list - list repositories at path */
int cmd_repository_list(int argc, char *argv[]);

/* Subcommand: local-dev - manage local development tracking */
int cmd_repository_local_dev(int argc, char *argv[]);

/*
 * Remove a local-dev package's registry entries (registry-local-dev.dat and
 * registry.dat) and its tracking directory, by (author, name, version).
 * Equivalent to `repository local-dev clear PACKAGE VERSION` but takes the
 * triple directly and produces no output. Does NOT remove the ELM_HOME cache
 * symlink for the package; callers manage that separately.
 */
void repository_clear_local_dev_registration(const char *author, const char *name,
                                             const char *version);

#endif /* REPOSITORY_H */
