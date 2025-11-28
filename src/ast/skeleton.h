/**
 * skeleton.h - Skeleton AST for Elm source files
 *
 * A skeleton AST contains only the structural information needed for
 * documentation generation and type analysis:
 *   - Module declaration (name, exports)
 *   - Imports
 *   - Type annotations
 *   - Type aliases
 *   - Union types
 *   - Infix declarations
 *   - Doc comments
 *
 * Implementation details (function bodies, expressions, patterns) are
 * intentionally omitted.
 */

#ifndef AST_SKELETON_H
#define AST_SKELETON_H

#include <tree_sitter/api.h>
#include <stdbool.h>

/* ============================================================================
 * Forward declarations
 * ========================================================================== */

typedef struct SkeletonModule SkeletonModule;
typedef struct SkeletonImport SkeletonImport;
typedef struct SkeletonTypeAnnotation SkeletonTypeAnnotation;
typedef struct SkeletonTypeAlias SkeletonTypeAlias;
typedef struct SkeletonUnionType SkeletonUnionType;
typedef struct SkeletonUnionConstructor SkeletonUnionConstructor;
typedef struct SkeletonInfix SkeletonInfix;
typedef struct SkeletonExports SkeletonExports;

/* ============================================================================
 * Export list
 * ========================================================================== */

struct SkeletonExports {
    bool expose_all;                      /* module Foo exposing (..) */

    char **values;                        /* Exposed value/function names */
    int values_count;
    int values_capacity;

    char **types;                         /* Exposed type names (without constructors) */
    int types_count;
    int types_capacity;

    char **types_with_constructors;       /* Types exposed with (..) */
    int types_with_constructors_count;
    int types_with_constructors_capacity;
};

/* ============================================================================
 * Import declaration
 * ========================================================================== */

struct SkeletonImport {
    char *module_name;                    /* e.g., "Json.Decode" */
    char *alias;                          /* e.g., "D" from "import Json.Decode as D" */
    bool expose_all;                      /* exposing (..) */

    char **exposed_values;                /* Values from exposing clause */
    int exposed_values_count;
    int exposed_values_capacity;

    char **exposed_types;                 /* Types from exposing clause */
    int exposed_types_count;
    int exposed_types_capacity;

    char **exposed_types_with_constructors;  /* Types exposed with (..) */
    int exposed_types_with_constructors_count;
    int exposed_types_with_constructors_capacity;
};

/* ============================================================================
 * Type annotation (function signature)
 * ========================================================================== */

struct SkeletonTypeAnnotation {
    char *name;                           /* Function/value name */
    TSNode type_node;                     /* AST node for type_expression */
    char *doc_comment;                    /* Preceding doc comment, or NULL */
    int implementation_param_count;       /* Number of params in implementation */

    /* After qualification/canonicalization */
    char *qualified_type;                 /* Qualified type string, or NULL if not yet processed */
    char *canonical_type;                 /* Final canonical form, or NULL if not yet processed */
};

/* ============================================================================
 * Type alias declaration
 * ========================================================================== */

struct SkeletonTypeAlias {
    char *name;                           /* Alias name */
    char **type_params;                   /* Type parameters (e.g., ["a", "b"]) */
    int type_params_count;
    int type_params_capacity;
    TSNode type_node;                     /* AST node for type_expression */
    char *doc_comment;                    /* Preceding doc comment, or NULL */

    /* After qualification/canonicalization */
    char *qualified_type;
    char *canonical_type;
};

/* ============================================================================
 * Union type constructor
 * ========================================================================== */

struct SkeletonUnionConstructor {
    char *name;                           /* Constructor name */
    TSNode *arg_nodes;                    /* AST nodes for constructor arguments */
    int arg_nodes_count;

    /* After qualification/canonicalization */
    char **qualified_args;
    char **canonical_args;
};

/* ============================================================================
 * Union type declaration
 * ========================================================================== */

struct SkeletonUnionType {
    char *name;                           /* Type name */
    char **type_params;                   /* Type parameters */
    int type_params_count;
    int type_params_capacity;
    SkeletonUnionConstructor *constructors;
    int constructors_count;
    char *doc_comment;                    /* Preceding doc comment, or NULL */
};

/* ============================================================================
 * Infix operator declaration
 * ========================================================================== */

struct SkeletonInfix {
    char *operator;                       /* Operator symbol, e.g., "|>" */
    char *function_name;                  /* Associated function name */
    int precedence;                       /* 0-9 */
    char *associativity;                  /* "left", "right", or "non" */

    /* The type comes from the function's type annotation, linked after parsing */
    SkeletonTypeAnnotation *type_annotation;  /* Points to corresponding annotation */
};

/* ============================================================================
 * Complete module skeleton
 * ========================================================================== */

struct SkeletonModule {
    /* Source information */
    char *filepath;                       /* Original file path */
    char *source_code;                    /* Source code (owned, normalized line endings) */
    TSTree *tree;                         /* Parsed AST tree (owned) */

    /* Module declaration */
    char *module_name;                    /* e.g., "Json.Decode" */
    SkeletonExports exports;              /* What the module exposes */
    char *module_doc_comment;             /* Module-level doc comment */

    /* Imports */
    SkeletonImport *imports;
    int imports_count;
    int imports_capacity;

    /* Declarations */
    SkeletonTypeAnnotation *type_annotations;
    int type_annotations_count;
    int type_annotations_capacity;

    SkeletonTypeAlias *type_aliases;
    int type_aliases_count;
    int type_aliases_capacity;

    SkeletonUnionType *union_types;
    int union_types_count;
    int union_types_capacity;

    SkeletonInfix *infixes;
    int infixes_count;
    int infixes_capacity;

    /* Local type names (for qualification) */
    char **local_types;
    int local_types_count;
    int local_types_capacity;
};

/* ============================================================================
 * Lifecycle functions
 * ========================================================================== */

/**
 * Parse an Elm source file into a skeleton AST.
 * Returns NULL on parse failure.
 */
SkeletonModule *skeleton_parse(const char *filepath);

/**
 * Parse Elm source from a string into a skeleton AST.
 * The source string is copied internally.
 * Returns NULL on parse failure.
 */
SkeletonModule *skeleton_parse_string(const char *source_code, const char *filepath);

/**
 * Free a skeleton module and all its contents.
 */
void skeleton_free(SkeletonModule *module);

/* ============================================================================
 * Query functions
 * ========================================================================== */

/**
 * Check if a value/function name is exported.
 */
bool skeleton_is_value_exported(const SkeletonModule *module, const char *name);

/**
 * Check if a type name is exported.
 */
bool skeleton_is_type_exported(const SkeletonModule *module, const char *name);

/**
 * Check if a type's constructors are exported.
 */
bool skeleton_is_type_exposed_with_constructors(const SkeletonModule *module, const char *name);

/**
 * Find a type annotation by function name.
 * Returns NULL if not found.
 */
SkeletonTypeAnnotation *skeleton_find_type_annotation(SkeletonModule *module, const char *name);

/**
 * Find a type alias by name.
 * Returns NULL if not found.
 */
SkeletonTypeAlias *skeleton_find_type_alias(SkeletonModule *module, const char *name);

/**
 * Find a union type by name.
 * Returns NULL if not found.
 */
SkeletonUnionType *skeleton_find_union_type(SkeletonModule *module, const char *name);

#endif /* AST_SKELETON_H */
