#ifndef CACHE_DOWNLOAD_MISSING_H
#define CACHE_DOWNLOAD_MISSING_H

/* Download missing dependencies from elm.json to cache
 *
 * Command: wrap package cache missing [OPTIONS] [PATH]
 *
 * Reads elm.json and identifies which dependencies are not yet downloaded
 * to the cache. By default, downloads directly from GitHub (faster, no
 * registry needed). For package projects with version constraints, use
 * --from-registry to resolve versions via the package registry.
 *
 * Download Sources:
 *   (default)       Download directly from GitHub (application projects only)
 *   --from-github   Same as default (explicit flag for clarity)
 *   --from-registry Use package registry for metadata and download
 *                   (required for package projects with version constraints)
 *
 * Options:
 *   -y, --yes       Skip confirmation prompt
 *   -v, --verbose   Show detailed progress
 *   -q, --quiet     Suppress progress messages
 *   --help          Show usage information
 *
 * Arguments:
 *   PATH            Optional path to directory containing elm.json
 *                   (defaults to current directory)
 *
 * Returns 0 on success, 1 on error
 */
int cmd_cache_download_missing(int argc, char *argv[]);

#endif /* CACHE_DOWNLOAD_MISSING_H */
