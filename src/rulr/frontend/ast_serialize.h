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
 * Serialize an AstProgram to a compressed binary format.
 * The output is compressed with deflate.
 *
 * @param prog The AST program to serialize
 * @param out_data Pointer to receive allocated buffer (caller must free with arena_free)
 * @param out_size Pointer to receive size of output data
 * @return Error structure indicating success or failure
 */
AstSerializeError ast_serialize(const AstProgram *prog, unsigned char **out_data, size_t *out_size);

/**
 * Deserialize a compressed binary format back to an AstProgram.
 *
 * @param data The compressed binary data
 * @param size Size of the input data
 * @param prog Pointer to AstProgram to populate (must be initialized)
 * @return Error structure indicating success or failure
 */
AstSerializeError ast_deserialize(const unsigned char *data, size_t size, AstProgram *prog);

/**
 * Write serialized AST to a file.
 *
 * @param prog The AST program to serialize
 * @param path Output file path
 * @return Error structure indicating success or failure
 */
AstSerializeError ast_serialize_to_file(const AstProgram *prog, const char *path);

/**
 * Read and deserialize AST from a file.
 *
 * @param path Input file path
 * @param prog Pointer to AstProgram to populate (must be initialized)
 * @return Error structure indicating success or failure
 */
AstSerializeError ast_deserialize_from_file(const char *path, AstProgram *prog);

/**
 * Deserialize AST from a memory buffer.
 * This is an alias for ast_deserialize for API consistency.
 *
 * @param data The compressed binary data
 * @param size Size of the input data
 * @param prog Pointer to AstProgram to populate (must be initialized)
 * @return Error structure indicating success or failure
 */
static inline AstSerializeError ast_deserialize_from_memory(
    const void *data, size_t size, AstProgram *prog) {
    return ast_deserialize((const unsigned char *)data, size, prog);
}

/**
 * Pretty-print an AstProgram to stdout in canonical format.
 *
 * @param prog The AST program to print
 */
void ast_pretty_print(const AstProgram *prog);

#endif /* AST_SERIALIZE_H */
