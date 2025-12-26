#ifndef AST_SERIALIZE_H
#define AST_SERIALIZE_H

#include "frontend/ast.h"
#include <stddef.h>

/* Magic bytes for compiled AST files */
#define AST_MAGIC "RULRAST1"
#define AST_MAGIC_LEN 8

/* Error structure for serialization operations */
typedef struct {
    int  is_error;
    char message[256];
} AstSerializeError;

/**
 * Deserialize an AstProgram from RAW (uncompressed) binary data.
 *
 * This function expects the binary payload WITHOUT the magic header
 * and compression. The host is responsible for:
 * 1. Reading the .dlc file
 * 2. Checking the magic header (AST_MAGIC)
 * 3. Decompressing the data
 * 4. Passing the raw bytes to this function
 *
 * See reference/ast_serialize_compress.c for decompression helpers.
 *
 * @param data Raw uncompressed binary data
 * @param size Size of the data
 * @param prog Pointer to AstProgram to populate (must be initialized)
 * @return Error structure indicating success or failure
 */
AstSerializeError ast_deserialize_raw(const unsigned char *data, size_t size, AstProgram *prog);

/**
 * Pretty-print an AstProgram to stdout in canonical format.
 *
 * @param prog The AST program to print
 */
void ast_pretty_print(const AstProgram *prog);

/*
 * The following functions are NOT in librulr.a - they require miniz.
 * See reference/ast_serialize_compress.c for implementations.
 *
 * AstSerializeError ast_serialize(const AstProgram *prog, unsigned char **out_data, size_t *out_size);
 * AstSerializeError ast_deserialize(const unsigned char *data, size_t size, AstProgram *prog);
 * AstSerializeError ast_serialize_to_file(const AstProgram *prog, const char *path);
 * AstSerializeError ast_deserialize_from_file(const char *path, AstProgram *prog);
 */

#endif /* AST_SERIALIZE_H */
