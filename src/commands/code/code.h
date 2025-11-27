#ifndef CODE_H
#define CODE_H

/* Main entry point for the 'code' command group */
int cmd_code(int argc, char *argv[]);

/* Subcommand: format - parse and canonicalize Elm source, output AST */
int cmd_code_format(int argc, char *argv[]);

#endif /* CODE_H */
