/**
 * ast_deserialize.h - .dlc file I/O and decompression for elm-wrap
 *
 * This header provides convenience wrappers for reading compiled Datalog rules.
 * It handles file I/O and decompression, delegating actual AST parsing to
 * librulr.a's ast_deserialize_raw().
 */

#ifndef ELM_WRAP_AST_DESERIALIZE_H
#define ELM_WRAP_AST_DESERIALIZE_H

/* Include librulr.a's AST serialization header for:
 * - AST_MAGIC, AST_MAGIC_LEN
 * - AstSerializeError
 * - ast_deserialize_raw()
 * - ast_pretty_print()
 */
#include "frontend/ast_serialize.h"

#include <stddef.h>

/**
 * Deserialize a compressed .dlc file from memory.
 *
 * Handles: magic header check, decompression with miniz,
 * then delegates to librulr.a's ast_deserialize_raw().
 *
 * @param data The compressed binary data (full .dlc format)
 * @param size Size of the input data
 * @param prog Pointer to AstProgram to populate (must be initialized)
 * @return Error structure indicating success or failure
 */
AstSerializeError ast_deserialize(const unsigned char *data, size_t size, AstProgram *prog);

/**
 * Read and deserialize AST from a .dlc file.
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

#endif /* ELM_WRAP_AST_DESERIALIZE_H */
