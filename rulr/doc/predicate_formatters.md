# Predicate Formatters

This document describes the predicate formatter system for displaying rulr query results in user-friendly formats.

## Concept

When rulr evaluates Datalog rules, the results are stored as **relations** — sets of tuples. By default, these are printed in a simple format:

```
predicate(arg1, arg2, ...)
```

For certain predicates, this default output is suboptimal. For example, a list of 55 file paths produces a wall of text that's hard to parse visually:

```
error(/Volumes/Devel/var/elm-wrap/packages/foo/1.0.0/src/cli/src/file.ts)
error(/Volumes/Devel/var/elm-wrap/packages/foo/1.0.0/src/cli/src/config.ts)
error(/Volumes/Devel/var/elm-wrap/packages/foo/1.0.0/src/cli/src/index.ts)
... 52 more lines ...
```

**Predicate formatters** transform relation data into more readable representations. The same data formatted as a tree:

```
/Volumes/Devel/var/elm-wrap/packages/foo/1.0.0/
├── src
│   └── cli
│       └── src
│           ├── config.ts
│           ├── file.ts
│           └── index.ts
```

This is analogous to **custom printers** in OCaml's toplevel or **pretty printers** in GDB — functions that know how to display specific data types in a human-friendly way.

## Architecture

### Current Implementation

The formatter system is implemented in `src/commands/review/reporter.c` and `reporter.h`. It provides:

1. **Configuration struct** for controlling output behavior
2. **Utility functions** for path manipulation
3. **Predicate-specific formatters** for known predicates
4. **Generic formatters** that auto-detect appropriate formatting

```
┌─────────────────────────────────────────────────────────────────┐
│                        review.c                                  │
│  (calls reporter functions after rulr evaluation)               │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       reporter.h                                 │
│  ReporterConfig, reporter_print_*, utility functions            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       reporter.c                                 │
│  Tree building, sorting, ASCII art rendering                    │
└─────────────────────────────────────────────────────────────────┘
```

### Key Components

#### ReporterConfig

```c
typedef struct {
    const char *base_path;     /* Base path to strip from file paths */
    int         use_tree;      /* If true, display as tree; if false, flat list */
    int         use_color;     /* If true, use ANSI colors (NYI) */
    int         max_depth;     /* Max tree depth to show (-1 = unlimited) */
} ReporterConfig;
```

#### Formatter Functions

| Function | Purpose |
|----------|---------|
| `reporter_print_file_tree()` | Generic tree formatter for file path arrays |
| `reporter_print_file_list()` | Flat list with shortened paths |
| `reporter_print_redundant_files()` | Formatter for `redundant_file` predicate |
| `reporter_print_errors()` | Smart formatter for `error` predicate |

## Tree Formatter

The tree formatter is the primary predicate formatter, designed for file path data.

### Algorithm

1. **Extract paths** from relation tuples (symbol values)
2. **Find common prefix** among all paths
3. **Build tree structure** by parsing path components
4. **Sort nodes** (directories first, then alphabetical)
5. **Render with ASCII art** using `├──`, `└──`, `│` characters

### Tree Node Structure

```c
typedef struct TreeNode {
    char *name;                /* Directory or file name */
    int is_file;               /* True if leaf node */
    struct TreeNode **children;
    int children_count;
    int children_capacity;
} TreeNode;
```

### Rendering Logic

The tree is rendered depth-first with proper prefix inheritance:

```c
static void tree_print_node(TreeNode *node, const char *prefix, 
                            int is_last, int depth, int max_depth) {
    // Print current node with branch character
    printf("%s%s%s\n", prefix, is_last ? "└── " : "├── ", node->name);
    
    // Build child prefix (continue vertical line if not last)
    char *child_prefix = /* prefix + (is_last ? "    " : "│   ") */;
    
    // Recursively print children
    for (int i = 0; i < node->children_count; i++) {
        tree_print_node(node->children[i], child_prefix, 
                        i == node->children_count - 1, depth + 1, max_depth);
    }
}
```

### Output Example

```
/path/to/package/
├── elm-stuff
│   └── 0.19.1
│       ├── Module.elmi
│       └── Module.elmo
├── src
│   └── cli
│       ├── src
│       │   ├── config.ts
│       │   └── index.ts
│       └── tests
│           └── spec.ts
└── docs.json
```

## Usage in Review Command

The review command uses formatters when printing results:

```c
/* Get relation views */
EngineRelationView error_view = rulr_get_relation(&rulr, "error");
EngineRelationView redundant_view = rulr_get_relation(&rulr, "redundant_file");

/* Smart deduplication: if error == redundant_file, show tree only once */
int skip_error_detail = (error_view.num_tuples == redundant_view.num_tuples);

if (error_view.num_tuples > 0) {
    if (skip_error_detail) {
        printf("Found %d error(s) (see redundant files below)\n", ...);
    } else {
        reporter_print_errors(&rulr, error_view, base_path);
    }
}

if (redundant_view.num_tuples > 0) {
    printf("⚠️  Redundant files (%d):\n", redundant_view.num_tuples);
    reporter_print_redundant_files(&rulr, redundant_view, base_path);
}
```

## API Reference

### Configuration

```c
ReporterConfig reporter_default_config(void);
```

Returns a default configuration with:
- `base_path = NULL` (auto-detect common prefix)
- `use_tree = 1` (tree mode enabled)
- `use_color = 0` (no ANSI colors)
- `max_depth = -1` (unlimited depth)

### Path Utilities

```c
char *reporter_find_common_prefix(const char **paths, int count);
```

Finds the longest common directory prefix among paths. Returns arena-allocated string.

```c
char *reporter_strip_prefix(const char *path, const char *base_path);
```

Strips base_path from path, returning the relative portion.

### Generic Formatters

```c
void reporter_print_file_tree(const ReporterConfig *cfg, 
                               const char **paths, int count);
```

Prints an array of file paths as a directory tree.

```c
void reporter_print_file_list(const ReporterConfig *cfg,
                               const char **paths, int count);
```

Prints paths as a flat list with optional prefix stripping.

### Predicate-Specific Formatters

```c
void reporter_print_redundant_files(const Rulr *rulr,
                                     EngineRelationView view,
                                     const char *base_path);
```

Formats the `redundant_file(path)` relation as a tree.

```c
void reporter_print_errors(const Rulr *rulr,
                            EngineRelationView view,
                            const char *base_path);
```

Smart formatter for `error(...)` that:
- Detects if arguments are file paths (start with `/`)
- Uses tree format for file-path errors
- Falls back to tuple format for other error types

## Future Directions

### Formatter Registry

A future enhancement could introduce a **formatter registry** where predicate formatters are registered by name:

```c
typedef void (*PredicateFormatter)(const Rulr *rulr, 
                                    EngineRelationView view,
                                    const ReporterConfig *cfg);

typedef struct {
    const char *predicate_name;
    PredicateFormatter formatter;
} FormatterEntry;

void reporter_register_formatter(const char *predicate, PredicateFormatter fn);
PredicateFormatter reporter_get_formatter(const char *predicate);
```

### Rule-Defined Formatting Hints

Rules could include formatting directives:

```datalog
.output redundant_file format=tree base_strip=true
.output dependency format=table columns=[author, package, version]
```

### Additional Formatters

| Formatter | Use Case |
|-----------|----------|
| **Table** | Multi-column data like dependencies |
| **JSON** | Machine-readable output |
| **Grouped** | Group by first argument |
| **Summary** | Counts and statistics |
| **Diff** | Before/after comparisons |

### Color Support

ANSI color codes for terminal output:

```c
typedef struct {
    // ... existing fields ...
    int use_color;
    const char *dir_color;    /* e.g., "\033[34m" for blue */
    const char *file_color;   /* e.g., "\033[0m" for default */
    const char *error_color;  /* e.g., "\033[31m" for red */
} ReporterConfig;
```

### Implementing a New Formatter

To add a new predicate formatter:

1. **Define the function** in `reporter.c`:

```c
void reporter_print_my_predicate(const Rulr *rulr,
                                  EngineRelationView view,
                                  const char *context) {
    const Tuple *tuples = (const Tuple *)view.tuples;
    for (int i = 0; i < view.num_tuples; i++) {
        // Format each tuple appropriately
    }
}
```

2. **Declare in header** (`reporter.h`):

```c
void reporter_print_my_predicate(const Rulr *rulr,
                                  EngineRelationView view,
                                  const char *context);
```

3. **Call from review command** (`review.c`):

```c
EngineRelationView my_view = rulr_get_relation(&rulr, "my_predicate");
if (my_view.pred_id >= 0 && my_view.num_tuples > 0) {
    reporter_print_my_predicate(&rulr, my_view, base_path);
}
```

## Design Principles

1. **Graceful degradation**: If a formatter can't handle data, fall back to default tuple printing
2. **Context awareness**: Use available context (base paths, terminal width) to improve output
3. **Zero allocation where possible**: Reuse arena allocator, avoid malloc in hot paths
4. **Composability**: Formatters should be combinable (e.g., grouped + tree)
5. **Consistency**: Similar data should look similar across different predicates
