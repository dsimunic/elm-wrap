#include "code.h"
#include "../publish/docs/tree_util.h"
#include "../../alloc.h"
#include "../../progname.h"
#include "../../ast/canonicalize.h"
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
        char *canonical = canonicalize_type_node(node, source);
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
        char *canonical = canonicalize_type_node(type_expr, source);
        
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
