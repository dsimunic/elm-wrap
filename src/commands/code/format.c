#include "code.h"
#include "../publish/docs/tree_util.h"
#include "../../alloc.h"
#include "../../progname.h"
#include <tree_sitter/api.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* External tree-sitter-elm language function */
extern const TSLanguage *tree_sitter_elm(void);

static void print_format_usage(void) {
    printf("Usage: %s code format <FILE> [OPTIONS]\n", program_name);
    printf("\n");
    printf("Parse an Elm source file and output canonicalized AST.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <FILE>             Path to Elm source file\n");
    printf("\n");
    printf("Options:\n");
    printf("  --types            Only show type annotations and their canonical form\n");
    printf("  --ast              Show full AST structure (default)\n");
    printf("  -h, --help         Show this help message\n");
}

/* Forward declaration for recursive printing */
static void print_node_recursive(TSNode node, const char *source, int depth, bool show_anonymous);

/* Print indentation */
static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
}

/* Canonicalize a type_expression node - this is where we apply elm-format style rules */
static char *canonicalize_type_expression(TSNode node, const char *source);

/* Check if a type_expression contains a function arrow at the top level */
static bool type_contains_arrow(TSNode node, const char *source) {
    (void)source;  /* Unused - we check node structure only */
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

/* Check if a type_expression is a tuple (has commas at top level) */
static bool type_is_tuple(TSNode node, const char *source) {
    (void)source;  /* Unused - we check node type only */
    const char *node_type = ts_node_type(node);
    return strcmp(node_type, "tuple_type") == 0;
}

/* Recursively canonicalize type expression, building a string */
static void canonicalize_type_to_buffer(TSNode node, const char *source, 
                                         char *buffer, size_t *pos, size_t max_len,
                                         bool in_function_arg_position);

/* Append string to buffer with bounds checking */
static void buffer_append(char *buffer, size_t *pos, size_t max_len, const char *str) {
    size_t len = strlen(str);
    if (*pos + len < max_len) {
        memcpy(buffer + *pos, str, len);
        *pos += len;
        buffer[*pos] = '\0';
    }
}

/* Append char to buffer with bounds checking */
static void buffer_append_char(char *buffer, size_t *pos, size_t max_len, char c) {
    if (*pos + 1 < max_len) {
        buffer[*pos] = c;
        (*pos)++;
        buffer[*pos] = '\0';
    }
}

/* Get node text and append to buffer */
static void buffer_append_node_text(char *buffer, size_t *pos, size_t max_len,
                                     TSNode node, const char *source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t len = end - start;
    
    if (*pos + len < max_len) {
        memcpy(buffer + *pos, source + start, len);
        *pos += len;
        buffer[*pos] = '\0';
    }
}

/* Canonicalize a type expression node to buffer */
static void canonicalize_type_to_buffer(TSNode node, const char *source, 
                                         char *buffer, size_t *pos, size_t max_len,
                                         bool in_function_arg_position) {
    const char *node_type = ts_node_type(node);
    
    /* Handle different node types */
    if (strcmp(node_type, "type_expression") == 0) {
        /* type_expression = type_expression_inner (-> type_expression_inner)* */
        uint32_t child_count = ts_node_child_count(node);
        
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);
            
            if (strcmp(child_type, "arrow") == 0) {
                buffer_append(buffer, pos, max_len, " -> ");
            } else if (ts_node_is_named(child)) {
                /* This is a type component (before or after an arrow) */
                /* Check if this child is a function type - if so, and we're in arg position
                 * (not the final return type), we need parens */
                bool child_has_arrow = type_contains_arrow(child, source);
                
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
                    buffer_append_char(buffer, pos, max_len, '(');
                    canonicalize_type_to_buffer(child, source, buffer, pos, max_len, true);
                    buffer_append_char(buffer, pos, max_len, ')');
                } else {
                    canonicalize_type_to_buffer(child, source, buffer, pos, max_len, true);
                }
            }
        }
    } else if (strcmp(node_type, "type_ref") == 0) {
        /* type_ref = upper_case_qid type_arg* */
        /* Type arguments that are complex (type applications, function types) need parens */
        uint32_t child_count = ts_node_child_count(node);
        bool first = true;
        
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);
            
            if (ts_node_is_named(child)) {
                if (!first) {
                    buffer_append_char(buffer, pos, max_len, ' ');
                }
                
                /* Check if this is a type argument that needs parentheses */
                /* Type arguments (after the first upper_case_qid) need parens if they are:
                 * 1. A type_expression containing a type_ref with arguments (type application)
                 * 2. A type_expression with arrows (function type)
                 * 3. A type_ref with arguments (nested type application)
                 */
                bool needs_parens = false;
                if (strcmp(child_type, "type_expression") == 0) {
                    bool has_arrow = type_contains_arrow(child, source);
                    if (has_arrow) {
                        needs_parens = true;
                    } else {
                        /* Check if the inner type_ref has type arguments */
                        /* This handles cases like (Maybe a) passed as arg to Hint */
                        uint32_t expr_child_count = ts_node_child_count(child);
                        for (uint32_t j = 0; j < expr_child_count; j++) {
                            TSNode expr_child = ts_node_child(child, j);
                            const char *expr_child_type = ts_node_type(expr_child);
                            if (strcmp(expr_child_type, "type_ref") == 0) {
                                /* Check if this type_ref has type arguments */
                                uint32_t ref_named_children = ts_node_named_child_count(expr_child);
                                if (ref_named_children > 1) {
                                    /* type_ref has arguments (more than just the type name) */
                                    needs_parens = true;
                                    break;
                                }
                            }
                        }
                    }
                } else if (strcmp(child_type, "type_ref") == 0) {
                    /* Nested type_ref - check if it has type arguments */
                    uint32_t ref_child_count = ts_node_named_child_count(child);
                    /* If type_ref has more than just the type name, it has arguments */
                    needs_parens = (ref_child_count > 1);
                }
                
                if (needs_parens) {
                    buffer_append_char(buffer, pos, max_len, '(');
                    canonicalize_type_to_buffer(child, source, buffer, pos, max_len, false);
                    buffer_append_char(buffer, pos, max_len, ')');
                } else {
                    canonicalize_type_to_buffer(child, source, buffer, pos, max_len, false);
                }
                first = false;
            }
        }
    } else if (strcmp(node_type, "upper_case_qid") == 0) {
        /* Qualified identifier - just output as-is */
        buffer_append_node_text(buffer, pos, max_len, node, source);
    } else if (strcmp(node_type, "type_variable") == 0 ||
               strcmp(node_type, "lower_case_identifier") == 0) {
        /* Type variable - output as-is */
        buffer_append_node_text(buffer, pos, max_len, node, source);
    } else if (strcmp(node_type, "record_type") == 0) {
        /* Record type { field : type, ... } */
        buffer_append(buffer, pos, max_len, "{ ");
        
        uint32_t child_count = ts_node_child_count(node);
        bool first_field = true;
        
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);
            
            if (strcmp(child_type, "field_type") == 0) {
                if (!first_field) {
                    buffer_append(buffer, pos, max_len, ", ");
                }
                canonicalize_type_to_buffer(child, source, buffer, pos, max_len, false);
                first_field = false;
            } else if (strcmp(child_type, "record_base_identifier") == 0) {
                /* Extensible record: { a | field : type } */
                buffer_append_node_text(buffer, pos, max_len, child, source);
                buffer_append(buffer, pos, max_len, " | ");
            }
        }
        
        buffer_append(buffer, pos, max_len, " }");
    } else if (strcmp(node_type, "field_type") == 0) {
        /* field : type */
        uint32_t child_count = ts_node_child_count(node);
        
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);
            
            if (strcmp(child_type, "lower_case_identifier") == 0) {
                buffer_append_node_text(buffer, pos, max_len, child, source);
                buffer_append(buffer, pos, max_len, " : ");
            } else if (strcmp(child_type, "type_expression") == 0) {
                /* Field type expression - function args here don't need parens around simple types */
                canonicalize_type_to_buffer(child, source, buffer, pos, max_len, true);
            }
        }
    } else if (strcmp(node_type, "tuple_type") == 0) {
        /* Tuple ( type, type, ... ) */
        buffer_append(buffer, pos, max_len, "( ");
        
        uint32_t child_count = ts_node_child_count(node);
        bool first = true;
        
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            
            if (ts_node_is_named(child) && 
                strcmp(ts_node_type(child), "type_expression") == 0) {
                if (!first) {
                    buffer_append(buffer, pos, max_len, ", ");
                }
                canonicalize_type_to_buffer(child, source, buffer, pos, max_len, false);
                first = false;
            }
        }
        
        buffer_append(buffer, pos, max_len, " )");
    } else {
        /* For parenthesized expressions or other nodes, check context */
        /* Look for anonymous "(" and ")" children indicating parentheses */
        bool is_parenthesized = false;
        TSNode inner_type = node;
        
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (!ts_node_is_named(child)) {
                uint32_t start = ts_node_start_byte(child);
                uint32_t end = ts_node_end_byte(child);
                if (end - start == 1 && source[start] == '(') {
                    is_parenthesized = true;
                }
            } else if (strcmp(ts_node_type(child), "type_expression") == 0) {
                inner_type = child;
            }
        }
        
        if (is_parenthesized && !ts_node_eq(inner_type, node)) {
            /* This is a parenthesized type expression */
            /* Determine if parens are necessary */
            bool has_arrow = type_contains_arrow(inner_type, source);
            bool is_tuple = type_is_tuple(inner_type, source);
            
            /* Check if it's the unit type () */
            uint32_t inner_start = ts_node_start_byte(inner_type);
            uint32_t inner_end = ts_node_end_byte(inner_type);
            bool is_unit = (inner_end == inner_start);  /* Empty content */
            
            bool needs_parens = has_arrow || is_tuple || is_unit || !in_function_arg_position;
            
            if (needs_parens) {
                buffer_append_char(buffer, pos, max_len, '(');
                canonicalize_type_to_buffer(inner_type, source, buffer, pos, max_len, false);
                buffer_append_char(buffer, pos, max_len, ')');
            } else {
                /* Omit unnecessary parens in function arg position */
                canonicalize_type_to_buffer(inner_type, source, buffer, pos, max_len, 
                                            in_function_arg_position);
            }
        } else {
            /* Unknown node type - output children recursively */
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                if (ts_node_is_named(child)) {
                    canonicalize_type_to_buffer(child, source, buffer, pos, max_len,
                                                in_function_arg_position);
                }
            }
        }
    }
}

/* Main canonicalization entry point */
static char *canonicalize_type_expression(TSNode node, const char *source) {
    size_t max_len = 4096;
    char *buffer = arena_malloc(max_len);
    size_t pos = 0;
    buffer[0] = '\0';
    
    canonicalize_type_to_buffer(node, source, buffer, &pos, max_len, false);
    
    return buffer;
}

/* Print AST node recursively */
static void print_node_recursive(TSNode node, const char *source, int depth, bool show_anonymous) {
    const char *type = ts_node_type(node);
    bool is_named = ts_node_is_named(node);
    
    if (!show_anonymous && !is_named) {
        return;
    }
    
    print_indent(depth);
    
    /* Get node text (for leaf nodes) */
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t len = end - start;
    
    uint32_t child_count = ts_node_named_child_count(node);
    
    if (child_count == 0 && len < 60) {
        /* Leaf node - show text inline */
        char *text = arena_malloc(len + 1);
        memcpy(text, source + start, len);
        text[len] = '\0';
        
        /* Replace newlines with spaces for display */
        for (uint32_t i = 0; i < len; i++) {
            if (text[i] == '\n') text[i] = ' ';
        }
        
        printf("%s%s: \"%s\"\n", is_named ? "" : "(", type, text);
        arena_free(text);
    } else {
        printf("%s%s\n", is_named ? "" : "(", type);
    }
    
    /* If this is a type_expression, also show canonical form */
    if (strcmp(type, "type_expression") == 0) {
        char *canonical = canonicalize_type_expression(node, source);
        print_indent(depth);
        printf("  [CANONICAL]: %s\n", canonical);
        arena_free(canonical);
    }
    
    /* Recurse into children */
    uint32_t full_child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < full_child_count; i++) {
        TSNode child = ts_node_child(node, i);
        print_node_recursive(child, source, depth + 1, show_anonymous);
    }
}

/* Find and print type annotations */
static void find_type_annotations(TSNode node, const char *source) {
    const char *type = ts_node_type(node);
    
    if (strcmp(type, "type_annotation") == 0) {
        /* Found a type annotation - extract and canonicalize */
        
        /* Get the function name (first lower_case_identifier child) */
        char *func_name = NULL;
        TSNode type_expr = node;
        
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);
            
            if (strcmp(child_type, "lower_case_identifier") == 0 && !func_name) {
                func_name = get_node_text(child, source);
            } else if (strcmp(child_type, "type_expression") == 0) {
                type_expr = child;
            }
        }
        
        /* Get original text */
        char *original = get_node_text(type_expr, source);
        
        /* Get canonical form */
        char *canonical = canonicalize_type_expression(type_expr, source);
        
        printf("\n=== %s ===\n", func_name ? func_name : "(anonymous)");
        printf("Original:  %s\n", original);
        printf("Canonical: %s\n", canonical);
        
        if (strcmp(original, canonical) != 0) {
            printf("           ^^^ DIFFERS ^^^\n");
        }
        
        if (func_name) arena_free(func_name);
        arena_free(original);
        arena_free(canonical);
    }
    
    /* Recurse into children */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        find_type_annotations(child, source);
    }
}

int cmd_code_format(int argc, char *argv[]) {
    bool show_types_only = false;
    bool show_ast = true;
    const char *file_path = NULL;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_format_usage();
            return 0;
        } else if (strcmp(argv[i], "--types") == 0) {
            show_types_only = true;
            show_ast = false;
        } else if (strcmp(argv[i], "--ast") == 0) {
            show_ast = true;
        } else if (argv[i][0] != '-') {
            file_path = argv[i];
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return 1;
        }
    }
    
    if (!file_path) {
        fprintf(stderr, "Error: No input file specified\n");
        print_format_usage();
        return 1;
    }
    
    /* Read source file */
    char *source = read_file_normalized(file_path);
    if (!source) {
        fprintf(stderr, "Error: Could not read file '%s'\n", file_path);
        return 1;
    }
    
    /* Create parser */
    TSParser *parser = ts_parser_new();
    if (!parser) {
        fprintf(stderr, "Error: Could not create parser\n");
        arena_free(source);
        return 1;
    }
    
    /* Set Elm language */
    if (!ts_parser_set_language(parser, tree_sitter_elm())) {
        fprintf(stderr, "Error: Could not set Elm language\n");
        ts_parser_delete(parser);
        arena_free(source);
        return 1;
    }
    
    /* Parse source */
    TSTree *tree = ts_parser_parse_string(parser, NULL, source, strlen(source));
    if (!tree) {
        fprintf(stderr, "Error: Could not parse file\n");
        ts_parser_delete(parser);
        arena_free(source);
        return 1;
    }
    
    TSNode root = ts_tree_root_node(tree);
    
    printf("=== File: %s ===\n\n", file_path);
    
    if (show_types_only) {
        printf("Type Annotations (Original vs Canonical):\n");
        printf("==========================================\n");
        find_type_annotations(root, source);
    } else if (show_ast) {
        printf("AST Structure:\n");
        printf("==============\n\n");
        print_node_recursive(root, source, 0, false);
    }
    
    /* Cleanup */
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    arena_free(source);
    
    return 0;
}
