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

/**
 * Initialize a package at a specific path (shared helper).
 *
 * Creates package structure from embedded templates, with optional
 * local-dev registration.
 *
 * @param target_dir          Directory where package will be created
 * @param package_spec        Package specification (author/name[@version])
 * @param register_local_dev  Whether to register for local development
 * @param auto_yes           Skip confirmation prompts
 * @return 0 on success, non-zero on failure
 */
int package_init_at_path(const char *target_dir, const char *package_spec,
                         bool register_local_dev, bool auto_yes);

/* Info command entry point */
int cmd_info(int argc, char *argv[], const char *invocation);

/* Upgrade command entry point */
int cmd_upgrade(int argc, char *argv[]);

#endif /* INSTALL_H */
