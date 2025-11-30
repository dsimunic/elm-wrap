#ifndef REVIEW_H
#define REVIEW_H

/* Main entry point for the 'review' command group */
int cmd_review(int argc, char *argv[]);

/* Subcommand: file - run rulr rules against an Elm file */
int cmd_review_file(int argc, char *argv[]);

/* Subcommand: package - run rulr rules against an Elm package directory */
int cmd_review_package(int argc, char *argv[]);

#endif /* REVIEW_H */
