#ifndef POLICY_H
#define POLICY_H

/**
 * Policy command group - manage and view rulr policy rules
 */

/* Main entry point for the 'policy' command group */
int cmd_policy(int argc, char *argv[]);

/* Subcommand: view - print rule source to stdout */
int cmd_policy_view(int argc, char *argv[]);

#endif /* POLICY_H */
