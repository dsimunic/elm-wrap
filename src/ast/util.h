/**
 * util.h - Common AST utilities
 *
 * Low-level utilities shared across AST processing modules.
 */

#ifndef AST_UTIL_H
#define AST_UTIL_H

#include "tree_sitter/api.h"
#include <stddef.h>
#include <stdbool.h>

/* ============================================================================
 * File I/O
 * ========================================================================== */

/**
 * Read a file and normalize line endings to \n.
 * Returns NULL on error.
 */
char *ast_read_file_normalized(const char *filepath);

/* ============================================================================
 * Node text extraction
 * ========================================================================== */

/**
 * Get the raw text of an AST node.
 */
char *ast_get_node_text(TSNode node, const char *source_code);

/**
 * Byte range for comment extraction.
 */
typedef struct {
    uint32_t start;
    uint32_t end;
} AstByteRange;

/**
 * Collect all comment ranges within a node.
 */
void ast_collect_comment_ranges(TSNode node, AstByteRange *ranges, int *count, int capacity);

/**
 * Extract text from a node, skipping embedded comments.
 */
char *ast_extract_text_skip_comments(TSNode node, const char *source_code);

/* ============================================================================
 * Buffer utilities
 * ========================================================================== */

/**
 * Append a string to a buffer with bounds checking.
 */
void ast_buffer_append(char *buffer, size_t *pos, size_t max_len, const char *str);

/**
 * Append a character to a buffer with bounds checking.
 */
void ast_buffer_append_char(char *buffer, size_t *pos, size_t max_len, char c);

/**
 * Append node text to a buffer with bounds checking.
 */
void ast_buffer_append_node_text(char *buffer, size_t *pos, size_t max_len,
                                  TSNode node, const char *source_code);

/* ============================================================================
 * Tree-sitter helpers
 * ========================================================================== */

/**
 * Get the tree-sitter Elm language.
 */
const TSLanguage *ast_elm_language(void);

/**
 * Create a new parser configured for Elm.
 * Returns NULL on failure.
 */
TSParser *ast_create_elm_parser(void);

/**
 * Find a named child by type.
 * Returns a null node if not found.
 */
TSNode ast_find_child_by_type(TSNode node, const char *type);

/**
 * Find all named children of a given type.
 * Returns count of found children.
 */
int ast_find_children_by_type(TSNode node, const char *type, TSNode *out_nodes, int max_count);

/**
 * Check if a node type matches.
 */
bool ast_node_is_type(TSNode node, const char *type);

/* ============================================================================
 * Identifier utilities
 * ========================================================================== */

/**
 * Check if a character is valid in an Elm identifier.
 */
bool ast_is_identifier_char(char c);

/**
 * Check if a string is a valid Elm upper-case identifier.
 */
bool ast_is_upper_identifier(const char *str);

/**
 * Check if a string is a valid Elm lower-case identifier.
 */
bool ast_is_lower_identifier(const char *str);

#endif /* AST_UTIL_H */
