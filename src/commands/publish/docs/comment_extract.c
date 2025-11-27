#include "comment_extract.h"
#include "tree_util.h"
#include "../../../alloc.h"
#include <string.h>

/* Helper function to clean documentation comment */
char *clean_comment(const char *raw_comment) {
    if (!raw_comment) return arena_strdup("");

    size_t len = strlen(raw_comment);
    if (len < 5) return arena_strdup("");  /* Too short to be {-| ... -} */

    /* Check if it starts with {-| and ends with -} */
    if (strncmp(raw_comment, "{-|", 3) != 0) {
        return arena_strdup("");
    }
    if (len < 5 || strcmp(raw_comment + len - 2, "-}") != 0) {
        return arena_strdup("");
    }

    /* Extract content between {-| and -} */
    size_t content_len = len - 5;  /* Remove {-| and -} */
    char *cleaned = arena_malloc(content_len + 1);
    if (!cleaned) return arena_strdup("");

    memcpy(cleaned, raw_comment + 3, content_len);
    cleaned[content_len] = '\0';

    return cleaned;
}

/* Helper function to find comment preceding a node */
char *find_preceding_comment(TSNode node, TSNode root, const char *source_code) {
    (void)root;

    /* Look for previous sibling that is a block_comment */
    TSNode prev_sibling = ts_node_prev_sibling(node);

    while (!ts_node_is_null(prev_sibling)) {
        const char *type = ts_node_type(prev_sibling);

        if (strcmp(type, "block_comment") == 0) {
            char *raw = get_node_text(prev_sibling, source_code);
            char *cleaned = clean_comment(raw);
            arena_free(raw);
            /* If this was a doc comment, return it. Otherwise continue searching. */
            if (cleaned && strlen(cleaned) > 0) {
                return cleaned;
            }
            arena_free(cleaned);
            /* Continue searching for a doc comment */
            prev_sibling = ts_node_prev_sibling(prev_sibling);
            continue;
        }

        /* Skip whitespace/newline/line_comment nodes */
        if (strcmp(type, "\n") != 0 && strcmp(type, "line_comment") != 0) {
            break;
        }

        prev_sibling = ts_node_prev_sibling(prev_sibling);
    }

    return arena_strdup("");
}
