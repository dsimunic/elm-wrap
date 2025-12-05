#ifndef APPLICATION_H
#define APPLICATION_H

/* Main entry point for the 'application' command group */
int cmd_application(int argc, char *argv[]);

/* Subcommand: init - initialize a new Elm application */
int cmd_application_init(int argc, char *argv[]);

/* Subcommand: info - display application information */
int cmd_application_info(int argc, char *argv[]);

#endif /* APPLICATION_H */
