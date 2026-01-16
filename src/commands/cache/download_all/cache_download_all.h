#ifndef CACHE_DOWNLOAD_ALL_H
#define CACHE_DOWNLOAD_ALL_H

/**
 * Download all packages from the Elm registry to cache.
 *
 * Synopsis:
 *   wrap package cache download-all [OPTIONS]
 *
 * This command will:
 *   - Update registry.dat with latest package information
 *   - Check each package version listed in the registry
 *   - Download any missing or broken packages
 *   - Fix broken packages (missing/empty src/) by re-downloading
 *
 * Options:
 *   -y, --yes           Skip confirmation prompt
 *   -q, --quiet         Suppress progress output
 *   -v, --verbose       Show detailed progress
 *   --dry-run           Show what would be downloaded without downloading
 *   --latest-only       Only download latest version of each package
 *   --help              Show help
 *
 * Returns 0 on success, non-zero on failure.
 */
int cmd_cache_download_all(int argc, char *argv[]);

#endif /* CACHE_DOWNLOAD_ALL_H */
