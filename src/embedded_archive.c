#include "embedded_archive.h"
#include "alloc.h"

#include <stdio.h>
#include <string.h>

/* Static state for the embedded archive */
static struct {
    bool initialized;
    bool available;
    mz_zip_archive zip;
    char *exe_path;
} g_archive = {0};

/**
 * Find the offset where the zip archive starts in the file.
 * Uses the end-of-central-directory record to locate the archive.
 */
static mz_uint64 find_zip_start(FILE *f, mz_uint64 file_size) {
    if (file_size < 22) {
        return 0;
    }

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
        return 0;
    }

    /* Read the EOCD to get the central directory offset */
    if (fseek(f, (long)eocd_pos, SEEK_SET) != 0) return 0;
    if (fread(buf, 1, 22, f) != 22) return 0;

    /* Central directory offset is at offset 16 in EOCD (4 bytes, little-endian) */
    mz_uint32 cdir_offset = buf[16] | (buf[17] << 8) | (buf[18] << 16) | (buf[19] << 24);
    mz_uint32 cdir_size = buf[12] | (buf[13] << 8) | (buf[14] << 16) | (buf[15] << 24);

    mz_uint64 zip_start = eocd_pos - cdir_offset - cdir_size;

    /* Sanity check: at zip_start there should be a local file header */
    if (fseek(f, (long)zip_start, SEEK_SET) != 0) return 0;
    if (fread(buf, 1, 4, f) != 4) return 0;

    if (buf[0] == 0x50 && buf[1] == 0x4B && buf[2] == 0x03 && buf[3] == 0x04) {
        return zip_start;
    }

    return 0;
}

bool embedded_archive_init(const char *exe_path) {
    if (g_archive.initialized) {
        return g_archive.available;
    }

    g_archive.initialized = true;
    g_archive.available = false;

    if (!exe_path) {
        return false;
    }

    g_archive.exe_path = arena_strdup(exe_path);
    if (!g_archive.exe_path) {
        return false;
    }

    FILE *f = fopen(exe_path, "rb");
    if (!f) {
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    mz_uint64 file_size = (mz_uint64)ftell(f);

    mz_uint64 zip_start = find_zip_start(f, file_size);
    fclose(f);

    if (zip_start == 0) {
        return false;
    }

    mz_uint64 archive_size = file_size - zip_start;

    mz_zip_zero_struct(&g_archive.zip);

    if (!mz_zip_reader_init_file_v2(&g_archive.zip, exe_path, 0, zip_start, archive_size)) {
        return false;
    }

    g_archive.available = true;
    return true;
}

bool embedded_archive_available(void) {
    return g_archive.available;
}

mz_uint embedded_archive_file_count(void) {
    if (!g_archive.available) {
        return 0;
    }
    return mz_zip_reader_get_num_files(&g_archive.zip);
}

bool embedded_archive_file_stat(mz_uint index, mz_zip_archive_file_stat *stat) {
    if (!g_archive.available || !stat) {
        return false;
    }
    return mz_zip_reader_file_stat(&g_archive.zip, index, stat);
}

bool embedded_archive_is_directory(mz_uint index) {
    if (!g_archive.available) {
        return false;
    }
    return mz_zip_reader_is_file_a_directory(&g_archive.zip, index);
}

int embedded_archive_locate(const char *name) {
    if (!g_archive.available || !name) {
        return -1;
    }
    return mz_zip_reader_locate_file(&g_archive.zip, name, NULL, 0);
}

bool embedded_archive_extract(const char *name, void **data, size_t *size) {
    if (!g_archive.available || !name || !data || !size) {
        return false;
    }

    *data = NULL;
    *size = 0;

    int index = embedded_archive_locate(name);
    if (index < 0) {
        return false;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&g_archive.zip, (mz_uint)index, &stat)) {
        return false;
    }

    *size = (size_t)stat.m_uncomp_size;
    *data = arena_malloc(*size);
    if (!*data) {
        return false;
    }

    if (!mz_zip_reader_extract_to_mem(&g_archive.zip, (mz_uint)index, *data, *size, 0)) {
        arena_free(*data);
        *data = NULL;
        *size = 0;
        return false;
    }

    return true;
}

void embedded_archive_cleanup(void) {
    if (g_archive.available) {
        mz_zip_reader_end(&g_archive.zip);
    }
    g_archive.initialized = false;
    g_archive.available = false;
    g_archive.exe_path = NULL;
}
