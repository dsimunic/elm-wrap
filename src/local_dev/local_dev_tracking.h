#ifndef LOCAL_DEV_TRACKING_H
#define LOCAL_DEV_TRACKING_H

#include <stdbool.h>

/**
 * local_dev_tracking.h - Shared local development package tracking API
 *
 * Provides functions for querying local-dev package tracking relationships:
 * - Which packages an application is tracking for local development
 * - Which applications are tracking a specific local-dev package
 */

/**
 * Information about a tracked local-dev package.
 */
typedef struct {
    char *author;
    char *name;
    char *version;
} LocalDevPackage;

/**
 * Get list of packages being tracked for local development by an application.
 *
 * Scans the tracking directory to find all local-dev packages that the
 * specified application's elm.json is registered to track.
 *
 * @param elm_json_path Path to the application's elm.json file
 * @param out_count     Output: number of packages found
 * @return Arena-allocated array of LocalDevPackage structs, or NULL if none found.
 *         Caller should free each package's strings and the array with arena_free().
 */
LocalDevPackage *local_dev_get_tracked_packages(const char *elm_json_path, int *out_count);

/**
 * Get list of application elm.json paths tracking a specific local-dev package.
 *
 * Scans the tracking directory to find all applications that have registered
 * to track the specified package version.
 *
 * @param author    Package author
 * @param name      Package name
 * @param version   Package version
 * @param out_count Output: number of application paths found
 * @return Arena-allocated array of paths, or NULL if none found.
 *         Caller should free each path and the array with arena_free().
 */
char **local_dev_get_tracking_apps(const char *author, const char *name,
                                   const char *version, int *out_count);

/**
 * Free a LocalDevPackage's contents.
 *
 * @param pkg Pointer to package to free (does not free the struct itself)
 */
void local_dev_package_free(LocalDevPackage *pkg);

/**
 * Free an array of LocalDevPackage structs and their contents.
 *
 * @param pkgs  Array of packages
 * @param count Number of packages in array
 */
void local_dev_packages_free(LocalDevPackage *pkgs, int count);

#endif /* LOCAL_DEV_TRACKING_H */
