#ifndef RULR_FILEUTIL_H
#define RULR_FILEUTIL_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Read entire file contents into an arena-allocated buffer, enforcing an
 * upper bound on file size.
 *
 * @param filepath Path to the file to read
 * @param max_bytes Maximum allowed file size in bytes
 * @param out_size Optional output for the number of bytes read (excluding NUL)
 *
 * @return Pointer to file contents (null-terminated), or NULL on failure
 *         (missing file, too large, read error, allocation failure).
 *         Caller should free with arena_free().
 */
char *file_read_contents_bounded(const char *filepath, size_t max_bytes, size_t *out_size);

/**
 * Read entire file contents into an arena-allocated buffer.
 * Uses MAX_FILE_READ_CONTENTS_BYTES as the size limit.
 *
 * @param filepath Path to the file to read
 * @return Pointer to file contents (null-terminated), or NULL on failure.
 */
char *file_read_contents(const char *filepath);

/**
 * Check if a regular file exists at the given path.
 *
 * @param path Path to check
 * @return true if file exists and is a regular file, false otherwise
 */
bool file_exists(const char *path);

#endif /* RULR_FILEUTIL_H */
