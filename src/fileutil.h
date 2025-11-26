#ifndef FILEUTIL_H
#define FILEUTIL_H

#include <stdbool.h>

/* Extract a ZIP file to a destination directory
 * Returns true on success, false on failure
 */
bool extract_zip(const char *zip_path, const char *dest_dir);

/* Extract specific files from a ZIP file to a destination directory
 * Only extracts: elm.json, docs.json, LICENSE, README.md, and src/ directory
 * Does not overwrite elm.json or docs.json if they already exist at destination
 * Returns true on success, false on failure
 */
bool extract_zip_selective(const char *zip_path, const char *dest_dir);

/* Find the first subdirectory in a directory
 * Returns allocated string with subdirectory path, or NULL if not found
 * Caller must free the returned string with arena_free()
 */
char* find_first_subdirectory(const char *dir_path);

/* Move contents of source directory to destination directory
 * This flattens the directory structure by one level
 * Returns true on success, false on failure
 */
bool move_directory_contents(const char *src_dir, const char *dest_dir);

/* Recursively delete a directory and all its contents
 * Returns true on success, false on failure
 */
bool remove_directory_recursive(const char *path);

/* Recursively copy a directory and all its contents
 * Returns true on success, false on failure
 */
bool copy_directory_recursive(const char *src_path, const char *dest_path);

/* Selectively copy specific files from source directory to destination directory
 * Only copies: elm.json, docs.json, LICENSE, README.md, and src/ directory
 * Returns true on success, false on failure
 */
bool copy_directory_selective(const char *src_path, const char *dest_path);

#endif /* FILEUTIL_H */
