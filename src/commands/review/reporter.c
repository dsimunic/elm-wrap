/**
 * reporter.c - Rulr result formatting and reporting utilities
 *
 * Implementation of tree-based file path display and predicate formatters.
 */

#include "reporter.h"
#include "../../alloc.h"
#include "../../dyn_array.h"
#include "../../messages.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Configuration
 * ========================================================================== */

ReporterConfig reporter_default_config(void) {
    ReporterConfig cfg;
    cfg.base_path = NULL;
    cfg.use_tree = 1;
    cfg.use_color = 0;
    cfg.show_base = 1;
    cfg.max_depth = -1;
    return cfg;
}

/* ============================================================================
 * File Path Utilities
 * ========================================================================== */

char *reporter_find_common_prefix(const char **paths, int count) {
    if (count == 0 || !paths || !paths[0]) {
        return NULL;
    }
    
    /* Start with the first path */
    int prefix_len = strlen(paths[0]);
    
    /* Find common prefix with each subsequent path */
    for (int i = 1; i < count; i++) {
        if (!paths[i]) continue;
        
        int j = 0;
        while (j < prefix_len && paths[i][j] && paths[0][j] == paths[i][j]) {
            j++;
        }
        prefix_len = j;
    }
    
    /* Trim to last directory separator */
    while (prefix_len > 0 && paths[0][prefix_len - 1] != '/') {
        prefix_len--;
    }
    
    /* Remove trailing slash unless it's the root */
    if (prefix_len > 1 && paths[0][prefix_len - 1] == '/') {
        prefix_len--;
    }
    
    if (prefix_len == 0) {
        return NULL;
    }
    
    char *result = arena_malloc(prefix_len + 1);
    if (!result) return NULL;
    
    memcpy(result, paths[0], prefix_len);
    result[prefix_len] = '\0';
    
    return result;
}

char *reporter_strip_prefix(const char *path, const char *base_path) {
    if (!path) return NULL;
    if (!base_path) return arena_strdup(path);
    
    int base_len = strlen(base_path);
    
    /* Check if path starts with base_path */
    if (strncmp(path, base_path, base_len) == 0) {
        /* Skip the base path and any leading slash */
        const char *rest = path + base_len;
        while (*rest == '/') rest++;
        return arena_strdup(rest);
    }
    
    return arena_strdup(path);
}

/* ============================================================================
 * Tree Node Structure
 * ========================================================================== */

typedef struct TreeNode {
    char *name;                /* Directory or file name (just the component) */
    int is_file;               /* True if this is a leaf (file) */
    struct TreeNode **children;
    int children_count;
    int children_capacity;
} TreeNode;

static TreeNode *tree_node_create(const char *name, int is_file) {
    TreeNode *node = arena_malloc(sizeof(TreeNode));
    if (!node) return NULL;
    
    node->name = arena_strdup(name);
    node->is_file = is_file;
    node->children = NULL;
    node->children_count = 0;
    node->children_capacity = 0;
    
    return node;
}

static TreeNode *tree_find_child(TreeNode *node, const char *name) {
    for (int i = 0; i < node->children_count; i++) {
        if (strcmp(node->children[i]->name, name) == 0) {
            return node->children[i];
        }
    }
    return NULL;
}

static void tree_add_child(TreeNode *parent, TreeNode *child) {
    if (parent->children_count >= parent->children_capacity) {
        int new_cap = parent->children_capacity == 0 ? 4 : parent->children_capacity * 2;
        parent->children = arena_realloc(parent->children, new_cap * sizeof(TreeNode*));
        parent->children_capacity = new_cap;
    }
    parent->children[parent->children_count++] = child;
}

static int tree_node_compare(const void *a, const void *b) {
    const TreeNode *na = *(const TreeNode **)a;
    const TreeNode *nb = *(const TreeNode **)b;
    
    /* Directories before files */
    if (na->is_file != nb->is_file) {
        return na->is_file - nb->is_file;
    }
    
    return strcmp(na->name, nb->name);
}

static void tree_sort_children(TreeNode *node) {
    if (node->children_count > 1) {
        qsort(node->children, node->children_count, sizeof(TreeNode*), tree_node_compare);
    }
    for (int i = 0; i < node->children_count; i++) {
        tree_sort_children(node->children[i]);
    }
}

static void tree_insert_path(TreeNode *root, const char *path) {
    TreeNode *current = root;
    const char *p = path;
    
    while (*p) {
        /* Skip leading slashes */
        while (*p == '/') p++;
        if (!*p) break;
        
        /* Find end of this component */
        const char *end = p;
        while (*end && *end != '/') end++;
        
        int len = end - p;
        char *component = arena_malloc(len + 1);
        memcpy(component, p, len);
        component[len] = '\0';
        
        int is_file = (*end == '\0');
        
        /* Find or create child */
        TreeNode *child = tree_find_child(current, component);
        if (!child) {
            child = tree_node_create(component, is_file);
            tree_add_child(current, child);
        } else if (is_file) {
            /* Mark as file if this is the final component */
            child->is_file = 1;
        }
        
        current = child;
        p = end;
    }
}

/* ============================================================================
 * Tree Printing
 * ========================================================================== */

static void tree_print_node(TreeNode *node, const char *prefix, int is_last, int depth, int max_depth) {
    if (max_depth >= 0 && depth > max_depth) return;
    
    if (node->name) {
        user_message("%s%s%s\n", prefix, is_last ? "└── " : "├── ", node->name);
    }
    
    /* Build prefix for children - need space for prefix + 4 chars for the branch + null terminator */
    size_t prefix_len = strlen(prefix);
    const char *suffix = (node->name ? (is_last ? "    " : "│   ") : "");
    size_t suffix_len = strlen(suffix);
    char *child_prefix = arena_malloc(prefix_len + suffix_len + 1);
    if (!child_prefix) return;

    memcpy(child_prefix, prefix, prefix_len);
    memcpy(child_prefix + prefix_len, suffix, suffix_len + 1);
    
    for (int i = 0; i < node->children_count; i++) {
        int child_is_last = (i == node->children_count - 1);
        tree_print_node(node->children[i], child_prefix, child_is_last, depth + 1, max_depth);
    }
}

void reporter_print_file_tree(const ReporterConfig *cfg, const char **paths, int count) {
    if (count == 0) {
        user_message("  (none)\n");
        return;
    }
    
    /* Find or use the provided base path */
    const char *base = cfg->base_path;
    char *computed_base = NULL;
    
    if (!base) {
        computed_base = reporter_find_common_prefix(paths, count);
        base = computed_base;
    }
    
    /* Print the base path as header */
    if (base && (!cfg || cfg->show_base)) {
        user_message("  %s/\n", base);
    }
    
    /* Build tree from stripped paths */
    TreeNode *root = tree_node_create(NULL, 0);
    
    for (int i = 0; i < count; i++) {
        char *rel_path = reporter_strip_prefix(paths[i], base);
        tree_insert_path(root, rel_path);
    }
    
    /* Sort the tree */
    tree_sort_children(root);
    
    /* Print tree */
    for (int i = 0; i < root->children_count; i++) {
        int is_last = (i == root->children_count - 1);
        tree_print_node(root->children[i], "  ", is_last, 0, cfg->max_depth);
    }
}

void reporter_print_file_list(const ReporterConfig *cfg, const char **paths, int count) {
    if (count == 0) {
        user_message("  (none)\n");
        return;
    }
    
    const char *base = cfg->base_path;
    char *computed_base = NULL;
    
    if (!base) {
        computed_base = reporter_find_common_prefix(paths, count);
        base = computed_base;
    }
    
    if (base) {
        user_message("  (relative to %s)\n", base);
    }
    
    for (int i = 0; i < count; i++) {
        char *rel_path = reporter_strip_prefix(paths[i], base);
        user_message("  - %s\n", rel_path);
    }
}

/* ============================================================================
 * Predicate-specific formatters
 * ========================================================================== */

void reporter_print_redundant_files(
    const Rulr *rulr,
    EngineRelationView view,
    const char *base_path
) {
    if (view.num_tuples == 0) {
        return;
    }
    
    /* Extract file paths from tuples */
    int paths_capacity = view.num_tuples;
    int paths_count = 0;
    const char **paths = arena_malloc(paths_capacity * sizeof(char*));
    
    const Tuple *tuples = (const Tuple *)view.tuples;
    for (int i = 0; i < view.num_tuples; i++) {
        const Tuple *t = &tuples[i];
        if (t->arity >= 1 && t->fields[0].kind == VAL_SYM) {
            const char *path = rulr_lookup_symbol(rulr, t->fields[0].u.sym);
            if (path) {
                paths[paths_count++] = path;
            }
        }
    }
    
    /* Print as tree */
    ReporterConfig cfg = reporter_default_config();
    cfg.base_path = base_path;
    cfg.use_tree = 1;
    
    reporter_print_file_tree(&cfg, paths, paths_count);
}

void reporter_print_errors(
    const Rulr *rulr,
    EngineRelationView view,
    const char *base_path
) {
    if (view.num_tuples == 0) {
        return;
    }
    
    /* Check if this looks like a file-path error (single symbol argument that looks like a path) */
    const Tuple *first = &((const Tuple *)view.tuples)[0];
    int is_file_error = 0;
    
    if (first->arity == 1 && first->fields[0].kind == VAL_SYM) {
        const char *val = rulr_lookup_symbol(rulr, first->fields[0].u.sym);
        if (val && val[0] == '/') {
            is_file_error = 1;
        }
    }
    
    if (is_file_error) {
        /* Treat as file list - extract paths and print as tree */
        int paths_capacity = view.num_tuples;
        int paths_count = 0;
        const char **paths = arena_malloc(paths_capacity * sizeof(char*));
        
        const Tuple *tuples = (const Tuple *)view.tuples;
        for (int i = 0; i < view.num_tuples; i++) {
            const Tuple *t = &tuples[i];
            if (t->arity >= 1 && t->fields[0].kind == VAL_SYM) {
                const char *path = rulr_lookup_symbol(rulr, t->fields[0].u.sym);
                if (path) {
                    paths[paths_count++] = path;
                }
            }
        }
        
        ReporterConfig cfg = reporter_default_config();
        cfg.base_path = base_path;
        reporter_print_file_tree(&cfg, paths, paths_count);
    } else {
        /* Fall back to simple tuple printing */
        const Tuple *tuples = (const Tuple *)view.tuples;
        for (int i = 0; i < view.num_tuples; i++) {
            const Tuple *t = &tuples[i];
            user_message("  error(");
            for (int a = 0; a < t->arity; a++) {
                const Value *v = &t->fields[a];
                switch (v->kind) {
                case VAL_SYM: {
                    const char *name = rulr_lookup_symbol(rulr, v->u.sym);
                    if (name) {
                        /* Strip base path if it's a file path */
                        if (base_path && strncmp(name, base_path, strlen(base_path)) == 0) {
                            const char *rest = name + strlen(base_path);
                            while (*rest == '/') rest++;
                            user_message("%s", rest);
                        } else {
                            user_message("%s", name);
                        }
                    } else {
                        user_message("#%d", v->u.sym);
                    }
                    break;
                }
                case VAL_INT:
                    user_message("%ld", v->u.i);
                    break;
                default:
                    user_message("?");
                    break;
                }
                if (a + 1 < t->arity) {
                    user_message(", ");
                }
            }
            user_message(")\n");
        }
    }
}
