/**
 * path_util.h - Shared path utilities for Elm documentation generation
 */

#ifndef PATH_UTIL_H
#define PATH_UTIL_H

/**
 * Convert an Elm module name to its relative file path.
 *
 * Example: "Eth.Sentry.Event" -> "Eth/Sentry/Event.elm"
 *
 * @param module_name The dot-separated module name
 * @return Newly allocated path string (caller must arena_free)
 */
char *module_name_to_file_path(const char *module_name);

#endif /* PATH_UTIL_H */
