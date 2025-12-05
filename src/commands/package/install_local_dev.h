#ifndef INSTALL_LOCAL_DEV_H
#define INSTALL_LOCAL_DEV_H

#include <stdbool.h>
#include "../../install_env.h"
#include "../../elm_json.h"

/**
 * Install a package for local development using symlinks.
 *
 * This creates a symlink in ELM_HOME pointing to the package source directory,
 * allowing changes to be immediately reflected without republishing.
 *
 * @param source_path     Absolute path to the package source directory
 * @param package_name    Package name in "author/name" format (optional, read from elm.json if NULL)
 * @param target_elm_json Path to the application's elm.json being modified
 * @param env             Install environment
 * @param is_test         Whether to install as test dependency
 * @param auto_yes        Skip confirmation prompts
 * @return 0 on success, non-zero on failure
 */
int install_local_dev(const char *source_path, const char *package_name,
                      const char *target_elm_json, InstallEnv *env,
                      bool is_test, bool auto_yes);

/**
 * Register a package for local development (cache + registry only, no app modification).
 *
 * This is used when running --local-dev from within the package directory itself.
 * It creates a symlink in ELM_HOME and registers the package in the local-dev registry,
 * but does NOT try to add the package as a dependency to any application.
 *
 * @param source_path   Absolute path to the package source directory
 * @param package_name  Package name in "author/name" format (optional, read from elm.json if NULL)
 * @param env           Install environment
 * @param auto_yes      Skip confirmation prompts
 * @return 0 on success, non-zero on failure
 */
int register_local_dev_package(const char *source_path, const char *package_name,
                               InstallEnv *env, bool auto_yes);

/**
 * Check if we're inside a package directory being developed and refresh
 * all dependent applications' indirect dependencies.
 *
 * Called after installing a new dependency to a package under development.
 *
 * @param env Install environment
 * @return 0 on success, non-zero on failure
 */
int refresh_local_dev_dependents(InstallEnv *env);

/**
 * Get the local-dev dependency tracking directory path.
 *
 * @return Arena-allocated path, or NULL on error
 */
char *get_local_dev_tracking_dir(void);

#endif /* INSTALL_LOCAL_DEV_H */
