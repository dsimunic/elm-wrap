#ifndef CACHE_DOWNLOAD_MISSING_H
#define CACHE_DOWNLOAD_MISSING_H

/* Download missing dependencies from elm.json to cache
 *
 * Command: wrap package cache missing [OPTIONS]
 *
 * Reads elm.json from the current directory and identifies which dependencies
 * are not yet downloaded to the cache. For applications, uses exact versions.
 * For packages, resolves version constraints to the latest matching version.
 *
 * Presents a plan of packages to download before proceeding.
 *
 * Options:
 *   -y, --yes       Skip confirmation prompt
 *   -v, --verbose   Show detailed progress
 *   -q, --quiet     Suppress progress messages
 *   --help          Show usage information
 *
 * Returns 0 on success, 1 on error
 */
int cmd_cache_download_missing(int argc, char *argv[]);

#endif /* CACHE_DOWNLOAD_MISSING_H */
