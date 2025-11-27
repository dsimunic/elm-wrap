#include "tree_util.h"
#include "../../../alloc.h"
#include <stdio.h>
#include <string.h>

/* Helper function to read file contents and normalize line endings to \n */
char *read_file_normalized(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        fprintf(stderr, "Error: Could not open file %s\n", filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = arena_malloc(file_size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(content, 1, file_size, file);
    content[read_size] = '\0';
    fclose(file);

    /* Normalize line endings: convert \r\n and \r to \n */
    char *normalized = arena_malloc(read_size + 1);
    size_t write_pos = 0;
    for (size_t i = 0; i < read_size; i++) {
        if (content[i] == '\r') {
            /* Skip \r - if followed by \n, we'll get it next iteration */
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

/* Helper function to get node text */
char *get_node_text(TSNode node, const char *source_code) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t length = end - start;

    char *text = arena_malloc(length + 1);
    if (!text) return NULL;

    memcpy(text, source_code + start, length);
    text[length] = '\0';
    return text;
}

/* Collect all comment ranges within a node */
void collect_comment_ranges(TSNode node, ByteRange *ranges, int *count, int capacity) {
    const char *node_type = ts_node_type(node);

    /* If this is a comment node, record its range */
    if (strcmp(node_type, "block_comment") == 0 || strcmp(node_type, "line_comment") == 0) {
        if (*count < capacity) {
            ranges[*count].start = ts_node_start_byte(node);
            ranges[*count].end = ts_node_end_byte(node);
            (*count)++;
        }
        return;  /* Don't recurse into comment nodes */
    }

    /* Recursively check children */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        collect_comment_ranges(child, ranges, count, capacity);
    }
}

/* Helper function to extract text from node, skipping comment ranges */
char *extract_text_skip_comments(TSNode node, const char *source_code) {
    uint32_t node_start = ts_node_start_byte(node);
    uint32_t node_end = ts_node_end_byte(node);
    uint32_t node_length = node_end - node_start;

    /* Collect all comment ranges within this node */
    ByteRange comment_ranges[64];
    int comment_count = 0;
    collect_comment_ranges(node, comment_ranges, &comment_count, 64);

    /* Allocate buffer for result */
    char *buffer = arena_malloc(node_length + 1);
    size_t pos = 0;

    /* Copy text, skipping comment ranges */
    uint32_t current = node_start;
    for (int i = 0; i < comment_count; i++) {
        /* Copy text before this comment */
        if (current < comment_ranges[i].start) {
            uint32_t len = comment_ranges[i].start - current;
            memcpy(buffer + pos, source_code + current, len);
            pos += len;
        }
        /* Skip the comment itself and move to after it */
        current = comment_ranges[i].end;
    }

    /* Copy remaining text after last comment */
    if (current < node_end) {
        uint32_t len = node_end - current;
        memcpy(buffer + pos, source_code + current, len);
        pos += len;
    }

    buffer[pos] = '\0';
    return buffer;
}

/* Helper function to count function arrows in type string (excluding arrows inside parens) */
int count_type_arrows(const char *type_str) {
    int arrow_count = 0;
    int paren_depth = 0;
    const char *p = type_str;

    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            paren_depth--;
        } else if (paren_depth == 0 && *p == '-' && *(p + 1) == '>' &&
                   (p > type_str && *(p - 1) == ' ') && *(p + 2) == ' ') {
            arrow_count++;
        }
        p++;
    }
    return arrow_count;
}

/* Helper function to count implementation parameters */
int count_implementation_params(TSNode value_decl_node, const char *source_code) {
    (void)source_code;
    int param_count = 0;

    /* Find function_declaration_left */
    uint32_t child_count = ts_node_child_count(value_decl_node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(value_decl_node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "function_declaration_left") == 0) {
            /* Count children that are parameters (lower_pattern, pattern, etc.) */
            /* Skip the first child which is the function name */
            uint32_t func_child_count = ts_node_child_count(child);
            bool found_func_name = false;
            for (uint32_t j = 0; j < func_child_count; j++) {
                TSNode func_child = ts_node_child(child, j);
                const char *func_child_type = ts_node_type(func_child);

                /* Skip the function name (first lower_case_identifier) */
                if (!found_func_name && strcmp(func_child_type, "lower_case_identifier") == 0) {
                    found_func_name = true;
                    continue;
                }

                /* Count anything that looks like a parameter */
                if (strcmp(func_child_type, "lower_pattern") == 0 ||
                    strcmp(func_child_type, "pattern") == 0 ||
                    strcmp(func_child_type, "lower_case_identifier") == 0 ||
                    strcmp(func_child_type, "anything_pattern") == 0 ||
                    strcmp(func_child_type, "tuple_pattern") == 0 ||
                    strcmp(func_child_type, "list_pattern") == 0 ||
                    strcmp(func_child_type, "record_pattern") == 0 ||
                    strcmp(func_child_type, "union_pattern") == 0) {
                    param_count++;
                }
            }
            break;
        }
    }

    return param_count;
}
