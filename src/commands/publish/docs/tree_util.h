#ifndef TREE_UTIL_H
#define TREE_UTIL_H

#include "tree_sitter/api.h"
#include <stdint.h>

/* Helper function to read file contents and normalize line endings to \n */
char *read_file_normalized(const char *filepath);

/* Helper function to get node text */
char *get_node_text(TSNode node, const char *source_code);

/* Helper function to collect comment byte ranges */
typedef struct {
    uint32_t start;
    uint32_t end;
} ByteRange;

/* Collect all comment ranges within a node */
void collect_comment_ranges(TSNode node, ByteRange *ranges, int *count, int capacity);

/* Helper function to extract text from node, skipping comment ranges */
char *extract_text_skip_comments(TSNode node, const char *source_code);

/* Helper function to count function arrows in type string (excluding arrows inside parens) */
int count_type_arrows(const char *type_str);

/* Helper function to count implementation parameters */
int count_implementation_params(TSNode value_decl_node, const char *source_code);

#endif /* TREE_UTIL_H */
