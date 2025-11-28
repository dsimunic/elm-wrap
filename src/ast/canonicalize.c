/**
 * canonicalize.c - AST-based type canonicalization implementation
 *
 * This implements elm-format style canonicalization for type expressions
 * by walking the AST directly rather than manipulating strings.
 */

#include "canonicalize.h"
#include "util.h"
#include "../alloc.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper predicates
 * ========================================================================== */

bool type_contains_arrow(TSNode node, const char *source_code) {
    (void)source_code;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *type = ts_node_type(child);
        if (strcmp(type, "arrow") == 0) {
            return true;
        }
    }
    return false;
}

bool type_is_tuple(TSNode node, const char *source_code) {
    (void)source_code;
    return strcmp(ts_node_type(node), "tuple_type") == 0;
}

bool type_is_record(TSNode node, const char *source_code) {
    (void)source_code;
    return strcmp(ts_node_type(node), "record_type") == 0;
}

bool type_is_application(TSNode node, const char *source_code) {
    (void)source_code;
    const char *node_type = ts_node_type(node);

    /* A type application is a type_ref with more than one named child */
    if (strcmp(node_type, "type_ref") == 0) {
        return ts_node_named_child_count(node) > 1;
    }

    /* Also check type_expression containing a type_ref with args */
    if (strcmp(node_type, "type_expression") == 0) {
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (strcmp(ts_node_type(child), "type_ref") == 0) {
                if (ts_node_named_child_count(child) > 1) {
                    return true;
                }
            }
        }
    }

    return false;
}

/* ============================================================================
 * Core canonicalization
 * ========================================================================== */

void canonicalize_type_to_buffer(TSNode node, const char *source_code,
                                  char *buffer, size_t *pos, size_t max_len,
                                  bool in_function_arg_position) {
    const char *node_type = ts_node_type(node);

    if (strcmp(node_type, "type_expression") == 0) {
        /* type_expression = type_expression_inner (-> type_expression_inner)* */
        uint32_t child_count = ts_node_child_count(node);

        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);

            if (strcmp(child_type, "arrow") == 0) {
                ast_buffer_append(buffer, pos, max_len, " -> ");
            } else if (ts_node_is_named(child)) {
                /* Check if this child is a function type */
                bool child_has_arrow = type_contains_arrow(child, source_code);

                /* Count remaining arrows to determine if this is the final return type */
                bool is_arg_position = false;
                for (uint32_t j = i + 1; j < child_count; j++) {
                    TSNode next = ts_node_child(node, j);
                    if (strcmp(ts_node_type(next), "arrow") == 0) {
                        is_arg_position = true;
                        break;
                    }
                }

                /* Need parens if child has arrow AND we're in argument position */
                if (child_has_arrow && is_arg_position) {
                    ast_buffer_append_char(buffer, pos, max_len, '(');
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, true);
                    ast_buffer_append_char(buffer, pos, max_len, ')');
                } else {
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, true);
                }
            }
        }
    } else if (strcmp(node_type, "type_ref") == 0) {
        /* type_ref = upper_case_qid type_arg* */
        uint32_t child_count = ts_node_child_count(node);
        bool first = true;

        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);

            if (ts_node_is_named(child)) {
                if (!first) {
                    ast_buffer_append_char(buffer, pos, max_len, ' ');
                }

                /* Check if this type argument needs parentheses */
                bool needs_parens = false;
                if (strcmp(child_type, "type_expression") == 0) {
                    bool has_arrow = type_contains_arrow(child, source_code);
                    if (has_arrow) {
                        needs_parens = true;
                    } else {
                        /* Check if the inner type_ref has type arguments */
                        uint32_t expr_child_count = ts_node_child_count(child);
                        for (uint32_t j = 0; j < expr_child_count; j++) {
                            TSNode expr_child = ts_node_child(child, j);
                            const char *expr_child_type = ts_node_type(expr_child);
                            if (strcmp(expr_child_type, "type_ref") == 0) {
                                uint32_t ref_named_children = ts_node_named_child_count(expr_child);
                                if (ref_named_children > 1) {
                                    needs_parens = true;
                                    break;
                                }
                            }
                        }
                    }
                } else if (strcmp(child_type, "type_ref") == 0) {
                    uint32_t ref_child_count = ts_node_named_child_count(child);
                    needs_parens = (ref_child_count > 1);
                }

                if (needs_parens) {
                    ast_buffer_append_char(buffer, pos, max_len, '(');
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, false);
                    ast_buffer_append_char(buffer, pos, max_len, ')');
                } else {
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, false);
                }
                first = false;
            }
        }
    } else if (strcmp(node_type, "upper_case_qid") == 0) {
        /* Qualified identifier - output as-is */
        ast_buffer_append_node_text(buffer, pos, max_len, node, source_code);
    } else if (strcmp(node_type, "type_variable") == 0 ||
               strcmp(node_type, "lower_case_identifier") == 0) {
        /* Type variable - output as-is */
        ast_buffer_append_node_text(buffer, pos, max_len, node, source_code);
    } else if (strcmp(node_type, "record_type") == 0) {
        /* Record type { field : type, ... } or empty record {} */
        uint32_t child_count = ts_node_child_count(node);

        /* Count fields and check for record_base_identifier */
        int field_count = 0;
        bool has_base = false;
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);
            if (strcmp(child_type, "field_type") == 0) {
                field_count++;
            } else if (strcmp(child_type, "record_base_identifier") == 0) {
                has_base = true;
            }
        }

        if (field_count == 0 && !has_base) {
            /* Empty record {} - no spaces */
            ast_buffer_append(buffer, pos, max_len, "{}");
        } else {
            /* Non-empty record with spaces */
            ast_buffer_append(buffer, pos, max_len, "{ ");

            bool first_field = true;
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                const char *child_type = ts_node_type(child);

                if (strcmp(child_type, "field_type") == 0) {
                    if (!first_field) {
                        ast_buffer_append(buffer, pos, max_len, ", ");
                    }
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, false);
                    first_field = false;
                } else if (strcmp(child_type, "record_base_identifier") == 0) {
                    /* Extensible record: { a | field : type } */
                    ast_buffer_append_node_text(buffer, pos, max_len, child, source_code);
                    ast_buffer_append(buffer, pos, max_len, " | ");
                }
            }

            ast_buffer_append(buffer, pos, max_len, " }");
        }
    } else if (strcmp(node_type, "field_type") == 0) {
        /* field : type */
        uint32_t child_count = ts_node_child_count(node);

        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);

            if (strcmp(child_type, "lower_case_identifier") == 0) {
                ast_buffer_append_node_text(buffer, pos, max_len, child, source_code);
                ast_buffer_append(buffer, pos, max_len, " : ");
            } else if (strcmp(child_type, "type_expression") == 0) {
                canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, true);
            }
        }
    } else if (strcmp(node_type, "tuple_type") == 0) {
        /* Tuple ( type, type, ... ) or unit type () */
        uint32_t child_count = ts_node_child_count(node);

        /* Count actual type expression children to detect unit type */
        int type_count = 0;
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (ts_node_is_named(child) &&
                strcmp(ts_node_type(child), "type_expression") == 0) {
                type_count++;
            }
        }

        if (type_count == 0) {
            /* Unit type () - no spaces */
            ast_buffer_append(buffer, pos, max_len, "()");
        } else {
            /* Regular tuple with spaces */
            ast_buffer_append(buffer, pos, max_len, "( ");

            bool first = true;
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);

                if (ts_node_is_named(child) &&
                    strcmp(ts_node_type(child), "type_expression") == 0) {
                    if (!first) {
                        ast_buffer_append(buffer, pos, max_len, ", ");
                    }
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len, false);
                    first = false;
                }
            }

            ast_buffer_append(buffer, pos, max_len, " )");
        }
    } else {
        /* Handle parenthesized expressions */
        bool is_parenthesized = false;
        TSNode inner_type = node;

        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (!ts_node_is_named(child)) {
                uint32_t start = ts_node_start_byte(child);
                uint32_t end = ts_node_end_byte(child);
                if (end - start == 1 && source_code[start] == '(') {
                    is_parenthesized = true;
                }
            } else if (strcmp(ts_node_type(child), "type_expression") == 0) {
                inner_type = child;
            }
        }

        if (is_parenthesized && !ts_node_eq(inner_type, node)) {
            bool has_arrow = type_contains_arrow(inner_type, source_code);
            bool is_tuple = type_is_tuple(inner_type, source_code);

            /* Check for unit type () */
            uint32_t inner_start = ts_node_start_byte(inner_type);
            uint32_t inner_end = ts_node_end_byte(inner_type);
            bool is_unit = (inner_end == inner_start);

            bool needs_parens = has_arrow || is_tuple || is_unit || !in_function_arg_position;

            if (needs_parens) {
                ast_buffer_append_char(buffer, pos, max_len, '(');
                canonicalize_type_to_buffer(inner_type, source_code, buffer, pos, max_len, false);
                ast_buffer_append_char(buffer, pos, max_len, ')');
            } else {
                canonicalize_type_to_buffer(inner_type, source_code, buffer, pos, max_len,
                                            in_function_arg_position);
            }
        } else {
            /* Unknown node type - recurse into children */
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                if (ts_node_is_named(child)) {
                    canonicalize_type_to_buffer(child, source_code, buffer, pos, max_len,
                                                in_function_arg_position);
                }
            }
        }
    }
}

char *canonicalize_type_node(TSNode node, const char *source_code) {
    size_t max_len = 65536;  /* 64KB - large records can exceed 4KB */
    char *buffer = arena_malloc(max_len);
    size_t pos = 0;
    buffer[0] = '\0';

    canonicalize_type_to_buffer(node, source_code, buffer, &pos, max_len, false);

    return buffer;
}
