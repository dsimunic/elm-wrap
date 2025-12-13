#ifndef INSTALL_H
#define INSTALL_H

#include <stdbool.h>

/* Install command entry point */
int cmd_install(int argc, char *argv[]);

/* Cache command entry point */
int cmd_cache(int argc, char *argv[]);

/* Remove command entry point */
int cmd_remove(int argc, char *argv[], const char *invocation);

/* Package init command entry point */
int cmd_package_init(int argc, char *argv[]);

/* Info command entry point */
int cmd_info(int argc, char *argv[], const char *invocation);

/* Upgrade command entry point */
int cmd_upgrade(int argc, char *argv[]);

#endif /* INSTALL_H */
