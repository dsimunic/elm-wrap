#ifndef INSTALL_LOCAL_DEV_H
#define INSTALL_LOCAL_DEV_H

#include <stdbool.h>
#include "../../install_env.h"
#include "../../elm_json.h"
#include "../../cache.h"

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
                      bool is_test, bool auto_yes, bool quiet);

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
 * @param quiet         Suppress plan output and success message (caller handles UI)
 * @return 0 on success, non-zero on failure
 */
int register_local_dev_package(const char *source_path, const char *package_name,
                               InstallEnv *env, bool auto_yes, bool quiet);

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
 * Check if we're inside a package directory being developed and prune
 * orphaned indirect dependencies from all dependent applications.
 *
 * Called after removing a dependency from a package under development.
 * This detects indirect dependencies that are no longer reachable from
 * any direct dependency and removes them.
 *
 * @param cache Cache configuration for looking up package elm.json files
 * @return 0 on success, non-zero on failure
 */
int prune_local_dev_dependents(CacheConfig *cache);

/**
 * Get the local-dev dependency tracking directory path.
 *
 * @return Arena-allocated path, or NULL on error
 */
char *get_local_dev_tracking_dir(void);

/**
 * Unregister a package from local development tracking.
 *
 * This removes the package from the local-dev tracking directory,
 * equivalent to running `wrap repository local-dev clear PACKAGE VERSION`.
 * Should be called from within the package directory.
 *
 * @param env Install environment (may be NULL if not needed for basic removal)
 * @return 0 on success, non-zero on failure
 */
int unregister_local_dev_package(InstallEnv *env);

/**
 * Check if a package is in the local-dev registry and register tracking if so.
 *
 * This is called after a regular `wrap install` command successfully installs
 * a package, to ensure that if the package is a local-dev package, the
 * application gets registered for tracking updates.
 *
 * @param author            Package author
 * @param name              Package name  
 * @param version           Package version
 * @param app_elm_json_path Path to the application's elm.json
 * @return true on success (or if package is not local-dev), false on error
 */
bool register_local_dev_tracking_if_needed(const char *author, const char *name,
                                           const char *version, const char *app_elm_json_path);

#endif /* INSTALL_LOCAL_DEV_H */
