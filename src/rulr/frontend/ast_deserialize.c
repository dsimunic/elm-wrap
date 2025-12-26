/**
 * ast_deserialize.c - .dlc file I/O and decompression for elm-wrap
 *
 * This file handles:
 * - File I/O for .dlc (compiled rule) files
 * - Decompression using miniz
 * - Delegates actual AST parsing to librulr.a's ast_deserialize_raw()
 *
 * The serialization format (.dlc) is:
 *   [8 bytes]  Magic: "RULRAST1"
 *   [4 bytes]  Uncompressed size (little-endian)
 *   [N bytes]  Deflate-compressed AST payload
 *
 * librulr.a provides ast_deserialize_raw() for parsing the uncompressed payload.
 */

#include "frontend/ast_deserialize.h"
#include "vendor/miniz.h"
#include "alloc.h"
#include "constants.h"
#include "fileutil.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Helper functions
 * ============================================================================ */

static AstSerializeError ast_err(const char *msg) {
    AstSerializeError err;
    err.is_error = 1;
    snprintf(err.message, sizeof(err.message), "%s", msg);
    return err;
}

/* ============================================================================
 * Deserialization - decompression wrapper around librulr.a
 * ============================================================================ */

AstSerializeError ast_deserialize(const unsigned char *data, size_t size, AstProgram *prog) {
    if (!data || !prog) {
        return ast_err("Invalid arguments");
    }

    /* Check magic header */
    if (size < AST_MAGIC_LEN + 4) {
        return ast_err("File too small");
    }
    if (memcmp(data, AST_MAGIC, AST_MAGIC_LEN) != 0) {
        return ast_err("Invalid magic header");
    }

    /* Read uncompressed size (little-endian) */
    unsigned int uncompressed_size =
        (unsigned int)(data[AST_MAGIC_LEN]) |
        ((unsigned int)(data[AST_MAGIC_LEN + 1]) << 8) |
        ((unsigned int)(data[AST_MAGIC_LEN + 2]) << 16) |
        ((unsigned int)(data[AST_MAGIC_LEN + 3]) << 24);

    /* Decompress with miniz */
    unsigned char *uncompressed = (unsigned char *)arena_malloc(uncompressed_size);
    if (!uncompressed) {
        return ast_err("Out of memory for decompression");
    }

    mz_ulong dest_len = uncompressed_size;
    int mz_result = mz_uncompress(
        uncompressed,
        &dest_len,
        data + AST_MAGIC_LEN + 4,
        (mz_ulong)(size - AST_MAGIC_LEN - 4)
    );

    if (mz_result != MZ_OK) {
        return ast_err("Decompression failed");
    }

    /* Delegate actual parsing to librulr.a */
    return ast_deserialize_raw(uncompressed, dest_len, prog);
}

AstSerializeError ast_deserialize_from_file(const char *path, AstProgram *prog) {
    size_t read_size = 0;
    char *data = file_read_contents_bounded(path, MAX_RULR_COMPILED_FILE_BYTES, &read_size);
    if (!data || read_size == 0) {
        arena_free(data);
        return ast_err("Failed to read file");
    }

    return ast_deserialize((const unsigned char *)data, read_size, prog);
}
