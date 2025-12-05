#ifndef EMBEDDED_ARCHIVE_H
#define EMBEDDED_ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>
#include "vendor/miniz.h"

/**
 * Initialize access to the embedded archive appended to the executable.
 * The archive is expected to be a ZIP file placed at the end of the binary.
 *
 * @param exe_path Absolute path to the running executable
 * @return true if the archive was found and opened successfully
 */
bool embedded_archive_init(const char *exe_path);

/**
 * Check if the embedded archive is available.
 */
bool embedded_archive_available(void);

/**
 * Get the number of entries in the embedded archive.
 * Returns 0 if the archive is not available.
 */
mz_uint embedded_archive_file_count(void);

/**
 * Retrieve file statistics for an entry in the embedded archive.
 *
 * @param index Index of the file (0-based)
 * @param stat  Output structure to populate
 * @return true on success
 */
bool embedded_archive_file_stat(mz_uint index, mz_zip_archive_file_stat *stat);

/**
 * Check if the entry at the given index is a directory.
 */
bool embedded_archive_is_directory(mz_uint index);

/**
 * Locate a file by name inside the embedded archive.
 *
 * @param name File path relative to the archive root
 * @return Index of the file, or -1 if not found
 */
int embedded_archive_locate(const char *name);

/**
 * Extract a file from the embedded archive into memory.
 * Memory is allocated with arena_malloc; caller should release with arena_free.
 *
 * @param name File path relative to the archive root
 * @param data Output pointer to extracted data
 * @param size Output size of extracted data
 * @return true on success
 */
bool embedded_archive_extract(const char *name, void **data, size_t *size);

/**
 * Clean up the embedded archive state.
 */
void embedded_archive_cleanup(void);

#endif /* EMBEDDED_ARCHIVE_H */
