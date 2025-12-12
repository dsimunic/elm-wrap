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

/* Check if a regular file exists at the given path
 * Returns true if file exists and is a regular file, false otherwise
 */
bool file_exists(const char *path);

/* Find elm.json by walking up parent directories.
 *
 * If start_path is NULL, starts from the current working directory.
 * If start_path is a file path, starts from its parent directory.
 * Returns an arena-allocated path to the nearest elm.json, or NULL if not found.
 */
char *find_elm_json_upwards(const char *start_path);

/* Read entire file contents into an arena-allocated buffer
 * Returns NULL on failure (file not found, read error, or allocation failure)
 * The returned string is null-terminated
 */
char *file_read_contents(const char *filepath);

/* Strip trailing slashes from a path
 * Returns an arena-allocated copy with trailing slashes removed
 * Returns NULL if path is NULL
 */
char *strip_trailing_slash(const char *path);

#endif /* FILEUTIL_H */
