/**
 * util.c - Common AST utilities implementation
 */

#include "util.h"
#include "../alloc.h"
#include "../constants.h"
#include "../fileutil.h"
#include <stdio.h>
#include <string.h>

/* External tree-sitter-elm language function */
extern const TSLanguage *tree_sitter_elm(void);

/* ============================================================================
 * File I/O
 * ========================================================================== */

char *ast_read_file_normalized(const char *filepath) {
    size_t read_size = 0;
    char *content = file_read_contents_bounded(filepath, MAX_ELM_SOURCE_FILE_BYTES, &read_size);
    if (!content) {
        fprintf(stderr, "Error: Could not read file %s\n", filepath);
        return NULL;
    }

    /* Normalize line endings: convert \r\n and \r to \n */
    char *normalized = arena_malloc(read_size + 1);
    if (!normalized) {
        arena_free(content);
        return NULL;
    }
    size_t write_pos = 0;
    for (size_t i = 0; i < read_size; i++) {
        if (content[i] == '\r') {
            if (i + 1 < read_size && content[i + 1] == '\n') {
                continue;  /* \r\n -> skip the \r, keep the \n */
            } else {
                normalized[write_pos++] = '\n';  /* standalone \r -> convert to \n */
            }
        } else {
            normalized[write_pos++] = content[i];
        }
    }
    normalized[write_pos] = '\0';

    arena_free(content);
    return normalized;
}

/* ============================================================================
 * Node text extraction
 * ========================================================================== */

char *ast_get_node_text(TSNode node, const char *source_code) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t length = end - start;

    char *text = arena_malloc(length + 1);
    if (!text) return NULL;

    memcpy(text, source_code + start, length);
    text[length] = '\0';
    return text;
}

void ast_collect_comment_ranges(TSNode node, AstByteRange *ranges, int *count, int capacity) {
    const char *node_type = ts_node_type(node);

    if (strcmp(node_type, "block_comment") == 0 || strcmp(node_type, "line_comment") == 0) {
        if (*count < capacity) {
            ranges[*count].start = ts_node_start_byte(node);
            ranges[*count].end = ts_node_end_byte(node);
            (*count)++;
        }
        return;
    }

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        ast_collect_comment_ranges(child, ranges, count, capacity);
    }
}

char *ast_extract_text_skip_comments(TSNode node, const char *source_code) {
    uint32_t node_start = ts_node_start_byte(node);
    uint32_t node_end = ts_node_end_byte(node);
    uint32_t node_length = node_end - node_start;

    AstByteRange comment_ranges[64];
    int comment_count = 0;
    ast_collect_comment_ranges(node, comment_ranges, &comment_count, 64);

    char *buffer = arena_malloc(node_length + 1);
    size_t pos = 0;

    uint32_t current = node_start;
    for (int i = 0; i < comment_count; i++) {
        if (current < comment_ranges[i].start) {
            uint32_t len = comment_ranges[i].start - current;
            memcpy(buffer + pos, source_code + current, len);
            pos += len;
        }
        current = comment_ranges[i].end;
    }

    if (current < node_end) {
        uint32_t len = node_end - current;
        memcpy(buffer + pos, source_code + current, len);
        pos += len;
    }

    buffer[pos] = '\0';
    return buffer;
}

/* ============================================================================
 * Buffer utilities
 * ========================================================================== */

void ast_buffer_append(char *buffer, size_t *pos, size_t max_len, const char *str) {
    size_t len = strlen(str);
    if (*pos + len < max_len) {
        memcpy(buffer + *pos, str, len);
        *pos += len;
        buffer[*pos] = '\0';
    }
}

void ast_buffer_append_char(char *buffer, size_t *pos, size_t max_len, char c) {
    if (*pos + 1 < max_len) {
        buffer[*pos] = c;
        (*pos)++;
        buffer[*pos] = '\0';
    }
}

void ast_buffer_append_node_text(char *buffer, size_t *pos, size_t max_len,
                                  TSNode node, const char *source_code) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t len = end - start;

    if (*pos + len < max_len) {
        memcpy(buffer + *pos, source_code + start, len);
        *pos += len;
        buffer[*pos] = '\0';
    }
}

/* ============================================================================
 * Tree-sitter helpers
 * ========================================================================== */

const TSLanguage *ast_elm_language(void) {
    return tree_sitter_elm();
}

TSParser *ast_create_elm_parser(void) {
    TSParser *parser = ts_parser_new();
    if (!parser) {
        return NULL;
    }

    if (!ts_parser_set_language(parser, tree_sitter_elm())) {
        ts_parser_delete(parser);
        return NULL;
    }

    return parser;
}

TSNode ast_find_child_by_type(TSNode node, const char *type) {
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), type) == 0) {
            return child;
        }
    }
    return (TSNode){0};  /* Null node */
}

int ast_find_children_by_type(TSNode node, const char *type, TSNode *out_nodes, int max_count) {
    int found = 0;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count && found < max_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), type) == 0) {
            out_nodes[found++] = child;
        }
    }
    return found;
}

bool ast_node_is_type(TSNode node, const char *type) {
    return strcmp(ts_node_type(node), type) == 0;
}

/* ============================================================================
 * Identifier utilities
 * ========================================================================== */

bool ast_is_identifier_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

bool ast_is_upper_identifier(const char *str) {
    if (!str || !*str) return false;
    if (*str < 'A' || *str > 'Z') return false;

    for (const char *p = str + 1; *p; p++) {
        if (!ast_is_identifier_char(*p)) return false;
    }
    return true;
}

bool ast_is_lower_identifier(const char *str) {
    if (!str || !*str) return false;
    if (*str < 'a' || *str > 'z') return false;

    for (const char *p = str + 1; *p; p++) {
        if (!ast_is_identifier_char(*p)) return false;
    }
    return true;
}
