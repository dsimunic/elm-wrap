/**
 * builtin_rules.c - Built-in rules embedded in the elm-wrap binary
 *
 * Reads pre-compiled rulr rules from a zip archive appended to the executable.
 * Uses miniz to read the zip archive from the tail of the binary.
 */

#include "builtin_rules.h"
#include "alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vendor/miniz.h"

/* Static state for the built-in rules subsystem */
static struct {
    bool initialized;
    bool available;
    mz_zip_archive zip;
    char *exe_path;
    
    /* Cached rule names (without .dlc extension) */
    int rule_count;
    char **rule_names;
} g_builtin = {0};

/**
 * Find the offset where the zip archive starts in the file.
 * ZIP files have a "PK\x03\x04" (local file header) signature at the start
 * of each file entry. We search backwards from the end to find where
 * the central directory points to, then use that to find the start.
 * 
 * Actually, for appended zips, the key is that the end of central directory
 * record stores the offset of the central directory relative to the start
 * of the ORIGINAL zip file. We need to find where the zip starts.
 * 
 * The simplest approach: search for "PK\x03\x04" (local file header signature)
 * scanning backwards from a reasonable distance before the end.
 */
static mz_uint64 find_zip_start(FILE *f, mz_uint64 file_size) {
    /* The end of central directory is within 64KB + 22 bytes of end */
    /* But the actual zip data could be anywhere */
    /* We'll search backwards for the first local file header (PK\x03\x04) */
    
    if (file_size < 22) {
        return 0; /* Too small to be a valid zip */
    }
    
    /* First, find the end of central directory record */
    mz_uint8 buf[4096];
    mz_uint64 search_start = (file_size > 65557) ? (file_size - 65557) : 0;
    mz_uint64 eocd_pos = 0;
    
    /* Scan backwards to find EOCD signature (PK\x05\x06) */
    for (mz_uint64 pos = file_size - 22; pos >= search_start; pos--) {
        if (fseek(f, (long)pos, SEEK_SET) != 0) break;
        if (fread(buf, 1, 4, f) != 4) break;
        
        if (buf[0] == 0x50 && buf[1] == 0x4B && buf[2] == 0x05 && buf[3] == 0x06) {
            eocd_pos = pos;
            break;
        }
    }
    
    if (eocd_pos == 0) {
        return 0; /* No EOCD found */
    }
    
    /* Read the EOCD to get the central directory offset */
    if (fseek(f, (long)eocd_pos, SEEK_SET) != 0) return 0;
    if (fread(buf, 1, 22, f) != 22) return 0;
    
    /* Central directory offset is at offset 16 in EOCD (4 bytes, little-endian) */
    mz_uint32 cdir_offset = buf[16] | (buf[17] << 8) | (buf[18] << 16) | (buf[19] << 24);
    
    /* Central directory size is at offset 12 in EOCD */
    mz_uint32 cdir_size = buf[12] | (buf[13] << 8) | (buf[14] << 16) | (buf[15] << 24);
    
    /* The zip starts at: eocd_pos - cdir_size - (distance from start to cdir_offset) */
    /* Wait, cdir_offset is relative to the start of the zip archive */
    /* So: actual_cdir_pos = zip_start + cdir_offset */
    /* And: eocd_pos = zip_start + cdir_offset + cdir_size */
    /* Therefore: zip_start = eocd_pos - cdir_offset - cdir_size */
    
    mz_uint64 zip_start = eocd_pos - cdir_offset - cdir_size;
    
    /* Sanity check: at zip_start there should be a local file header */
    if (fseek(f, (long)zip_start, SEEK_SET) != 0) return 0;
    if (fread(buf, 1, 4, f) != 4) return 0;
    
    if (buf[0] == 0x50 && buf[1] == 0x4B && buf[2] == 0x03 && buf[3] == 0x04) {
        return zip_start;
    }
    
    /* If sanity check fails, there might not be a valid zip */
    return 0;
}

bool builtin_rules_init(const char *exe_path) {
    if (g_builtin.initialized) {
        return g_builtin.available;
    }
    
    g_builtin.initialized = true;
    g_builtin.available = false;
    
    if (!exe_path) {
        return false;
    }
    
    /* Store the exe path */
    g_builtin.exe_path = arena_strdup(exe_path);
    if (!g_builtin.exe_path) {
        return false;
    }
    
    /* Open the file to find where the zip archive starts */
    FILE *f = fopen(exe_path, "rb");
    if (!f) {
        return false;
    }
    
    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    mz_uint64 file_size = (mz_uint64)ftell(f);
    
    /* Find where the zip archive starts */
    mz_uint64 zip_start = find_zip_start(f, file_size);
    fclose(f);
    
    if (zip_start == 0) {
        /* No valid zip found at end of file */
        return false;
    }
    
    /* Calculate the archive size (from zip_start to end of file) */
    mz_uint64 archive_size = file_size - zip_start;
    
    /* Initialize zip reader */
    mz_zip_zero_struct(&g_builtin.zip);
    
    /*
     * Open the zip archive with the correct offset and size.
     * file_start_ofs = where the zip data starts in the file
     * archive_size = size of the zip archive
     */
    if (!mz_zip_reader_init_file_v2(&g_builtin.zip, exe_path, 0, zip_start, archive_size)) {
        /* Failed to initialize zip reader */
        return false;
    }
    
    g_builtin.available = true;
    
    /* Count and cache rule names */
    mz_uint num_files = mz_zip_reader_get_num_files(&g_builtin.zip);
    
    /* Allocate space for rule names */
    int names_capacity = 16;
    g_builtin.rule_names = arena_malloc(names_capacity * sizeof(char*));
    g_builtin.rule_count = 0;
    
    if (!g_builtin.rule_names) {
        mz_zip_reader_end(&g_builtin.zip);
        g_builtin.available = false;
        return false;
    }
    
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&g_builtin.zip, i, &stat)) {
            continue;
        }
        
        /* Skip directories */
        if (mz_zip_reader_is_file_a_directory(&g_builtin.zip, i)) {
            continue;
        }
        
        /* Only include .dlc files */
        size_t name_len = strlen(stat.m_filename);
        if (name_len < 4 || strcmp(stat.m_filename + name_len - 4, ".dlc") != 0) {
            continue;
        }
        
        /* Grow array if needed */
        if (g_builtin.rule_count >= names_capacity) {
            names_capacity *= 2;
            g_builtin.rule_names = arena_realloc(g_builtin.rule_names, 
                                                  names_capacity * sizeof(char*));
            if (!g_builtin.rule_names) {
                mz_zip_reader_end(&g_builtin.zip);
                g_builtin.available = false;
                return false;
            }
        }
        
        /* Store name without .dlc extension */
        char *name = arena_malloc(name_len - 4 + 1);
        if (!name) {
            continue;
        }
        memcpy(name, stat.m_filename, name_len - 4);
        name[name_len - 4] = '\0';
        
        g_builtin.rule_names[g_builtin.rule_count++] = name;
    }
    
    return true;
}

bool builtin_rules_available(void) {
    return g_builtin.available;
}

bool builtin_rules_has(const char *name) {
    if (!g_builtin.available || !name) {
        return false;
    }
    
    /* Build the filename with .dlc extension */
    size_t name_len = strlen(name);
    char *filename = arena_malloc(name_len + 5);
    if (!filename) {
        return false;
    }
    snprintf(filename, name_len + 5, "%s.dlc", name);
    
    int index = mz_zip_reader_locate_file(&g_builtin.zip, filename, NULL, 0);
    arena_free(filename);
    
    return index >= 0;
}

bool builtin_rules_extract(const char *name, void **data, size_t *size) {
    if (!g_builtin.available || !name || !data || !size) {
        return false;
    }
    
    /* Build the filename with .dlc extension */
    size_t name_len = strlen(name);
    char *filename = arena_malloc(name_len + 5);
    if (!filename) {
        return false;
    }
    snprintf(filename, name_len + 5, "%s.dlc", name);
    
    /* Find the file in the archive */
    int index = mz_zip_reader_locate_file(&g_builtin.zip, filename, NULL, 0);
    arena_free(filename);
    
    if (index < 0) {
        return false;
    }
    
    /* Get file info */
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&g_builtin.zip, (mz_uint)index, &stat)) {
        return false;
    }
    
    /* Allocate buffer for the file */
    *size = (size_t)stat.m_uncomp_size;
    *data = arena_malloc(*size);
    if (!*data) {
        return false;
    }
    
    /* Extract the file to memory */
    if (!mz_zip_reader_extract_to_mem(&g_builtin.zip, (mz_uint)index, *data, *size, 0)) {
        arena_free(*data);
        *data = NULL;
        *size = 0;
        return false;
    }
    
    return true;
}

int builtin_rules_count(void) {
    if (!g_builtin.available) {
        return 0;
    }
    return g_builtin.rule_count;
}

const char *builtin_rules_name(int index) {
    if (!g_builtin.available || index < 0 || index >= g_builtin.rule_count) {
        return NULL;
    }
    return g_builtin.rule_names[index];
}

void builtin_rules_cleanup(void) {
    if (g_builtin.available) {
        mz_zip_reader_end(&g_builtin.zip);
    }
    g_builtin.initialized = false;
    g_builtin.available = false;
    g_builtin.rule_count = 0;
    g_builtin.rule_names = NULL;
    g_builtin.exe_path = NULL;
}
