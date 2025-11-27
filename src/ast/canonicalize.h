/**
 * canonicalize.h - AST-based type canonicalization
 *
 * Provides functions to canonicalize Elm type expressions according to
 * elm-format conventions:
 *   - Remove unnecessary parentheses in function argument positions
 *   - Preserve parentheses around function types in argument position
 *   - Normalize tuple and record spacing
 *   - Handle unit type ()
 */

#ifndef AST_CANONICALIZE_H
#define AST_CANONICALIZE_H

#include <tree_sitter/api.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Core canonicalization
 * ========================================================================== */

/**
 * Canonicalize a type_expression AST node to a string.
 *
 * This applies elm-format style rules:
 *   - Parentheses around function types in argument position: (a -> b) -> c
 *   - No parentheses around simple types in argument position: Maybe a -> b
 *   - Parentheses around type applications in argument position: (Maybe a) -> b
 *   - Consistent tuple spacing: ( a, b )
 *   - Consistent record spacing: { field : Type }
 *
 * @param node        The type_expression node to canonicalize
 * @param source_code The source code the node refers to
 * @return            Newly allocated canonical string (caller must free)
 */
char *canonicalize_type_node(TSNode node, const char *source_code);

/**
 * Canonicalize a type_expression node to a buffer.
 *
 * Same as canonicalize_type_node but writes to a provided buffer.
 *
 * @param node                   The type_expression node
 * @param source_code            The source code
 * @param buffer                 Output buffer
 * @param pos                    Current position in buffer (updated)
 * @param max_len                Maximum buffer length
 * @param in_function_arg_position Whether we're in a function argument position
 */
void canonicalize_type_to_buffer(TSNode node, const char *source_code,
                                  char *buffer, size_t *pos, size_t max_len,
                                  bool in_function_arg_position);

/* ============================================================================
 * Helper predicates
 * ========================================================================== */

/**
 * Check if a type_expression contains a function arrow at the top level.
 */
bool type_contains_arrow(TSNode node, const char *source_code);

/**
 * Check if a type_expression is a tuple type.
 */
bool type_is_tuple(TSNode node, const char *source_code);

/**
 * Check if a type_expression is a record type.
 */
bool type_is_record(TSNode node, const char *source_code);

/**
 * Check if a type_expression is a type application (type with arguments).
 */
bool type_is_application(TSNode node, const char *source_code);

#endif /* AST_CANONICALIZE_H */
