#ifndef CACHE_CHECK_H
#define CACHE_CHECK_H

#include <stdbool.h>

/* Check cache status for a specific package
 * Lists known versions from registry.dat and cached versions
 * Reports broken packages (missing or empty src/ directory)
 *
 * If purge_broken is true, removes broken directories without downloading
 * If fix_broken is true, attempts to download broken versions from registry/github
 *
 * package_name format: "author/name"
 * Returns 0 on success, 1 on error
 */
int cache_check_package(const char *package_name, bool purge_broken, bool fix_broken, bool verbose);

/* Entry point for the cache check command with package argument */
int cmd_cache_check(int argc, char *argv[]);

#endif /* CACHE_CHECK_H */
