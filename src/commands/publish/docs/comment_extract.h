#ifndef COMMENT_EXTRACT_H
#define COMMENT_EXTRACT_H

#include "tree_sitter/api.h"

/* Helper function to clean documentation comment
 * Removes {-| and -} markers and returns the inner content */
char *clean_comment(const char *raw_comment);

/* Helper function to find comment preceding a node
 * Returns the cleaned doc comment text or an empty string */
char *find_preceding_comment(TSNode node, TSNode root, const char *source_code);

#endif /* COMMENT_EXTRACT_H */
