#ifndef MESSAGES_H
#define MESSAGES_H

#include <stdio.h>

/*
 * user_message: unified macro for writing user-visible messages to stderr.
 * Using a macro keeps printf/fprintf usage consistent and centralized.
 */
#define user_message(...) fprintf(stderr, __VA_ARGS__)

#endif /* MESSAGES_H */
