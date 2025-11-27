#ifndef CACHE_FULL_SCAN_H
#define CACHE_FULL_SCAN_H

/* Full scan of the package cache
 * Scans all packages in the cache and verifies their status
 *
 * By default, shows information only for broken packages
 * With -q, only reports total counts
 *
 * Returns 0 on success, 1 on error
 */
int cmd_cache_full_scan(int argc, char *argv[]);

#endif /* CACHE_FULL_SCAN_H */
