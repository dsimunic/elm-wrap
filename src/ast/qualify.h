/**
 * qualify.h - Type name qualification for Elm AST
 *
 * Provides functions to qualify type names in Elm source according to
 * docs.json conventions:
 *   - Unqualified imported types get their module prefix: Maybe -> Maybe.Maybe
 *   - Local types remain unqualified
 *   - Already-qualified types remain unchanged
 *   - Module aliases are expanded: D.Value -> Json.Decode.Value
 */

#ifndef AST_QUALIFY_H
#define AST_QUALIFY_H

#include "skeleton.h"
#include <stdbool.h>

/* Forward declaration for dependency cache (if not already defined) */
#ifndef DEPENDENCY_CACHE_H
typedef struct DependencyCache DependencyCache;
#endif

/* Forward declarations for type_maps structures (if not already defined) */
#ifndef TYPE_MAPS_H
typedef struct ImportMap ImportMap;
typedef struct ModuleAliasMap ModuleAliasMap;
typedef struct DirectModuleImports DirectModuleImports;
#endif

/* ============================================================================
 * Import resolution context
 * ========================================================================== */

/**
 * Maps an unqualified type name to its source module.
 */
typedef struct {
    char *type_name;       /* The type name as used in this module */
    char *module_name;     /* The module it comes from */
} QualifyImportEntry;

/**
 * Maps a module alias to its full module name(s).
 * An alias can map to multiple modules (ambiguous until type is resolved).
 */
typedef struct {
    char *alias;               /* The alias, e.g., "D" */
    char **full_modules;       /* All modules using this alias */
    int full_modules_count;
    int full_modules_capacity;
} QualifyAliasEntry;

/**
 * Context for type qualification.
 * Built from a skeleton's imports.
 */
typedef struct {
    /* Type name -> module mapping (from exposing clauses) */
    QualifyImportEntry *imports;
    int imports_count;
    int imports_capacity;

    /* Alias -> module mapping */
    QualifyAliasEntry *aliases;
    int aliases_count;
    int aliases_capacity;

    /* Directly imported modules (available for qualified access) */
    char **direct_modules;
    int direct_modules_count;
    int direct_modules_capacity;

    /* Local types (defined in this module, should not be qualified) */
    char **local_types;
    int local_types_count;

    /* Current module name */
    char *current_module;

    /* Dependency cache for resolving ambiguous aliases */
    DependencyCache *dep_cache;
} QualifyContext;

/* ============================================================================
 * Context lifecycle
 * ========================================================================== */

/**
 * Create a qualification context from a skeleton module.
 * Applies implicit imports automatically.
 */
QualifyContext *qualify_context_create(SkeletonModule *skeleton, DependencyCache *dep_cache);

/**
 * Create a qualification context from existing type_maps structures.
 * This allows integration with existing docs pipeline code.
 * Note: local_types is borrowed (not copied) - caller must keep it alive.
 */
QualifyContext *qualify_context_create_from_maps(const char *module_name,
                                                  ImportMap *import_map,
                                                  ModuleAliasMap *alias_map,
                                                  DirectModuleImports *direct_imports,
                                                  char **local_types, int local_types_count,
                                                  DependencyCache *dep_cache);

/**
 * Free a qualification context.
 */
void qualify_context_free(QualifyContext *ctx);

/* ============================================================================
 * Type qualification
 * ========================================================================== */

/**
 * Qualify all types in a skeleton module.
 * Populates the qualified_type fields in annotations, aliases, and unions.
 */
void qualify_skeleton_types(SkeletonModule *skeleton, QualifyContext *ctx);

/**
 * Canonicalize all types in a skeleton module.
 * Populates the canonical_type fields using combined qualification and canonicalization.
 * This produces the final form suitable for docs.json output.
 */
void canonicalize_skeleton_types(SkeletonModule *skeleton, QualifyContext *ctx);

/**
 * Qualify a single type expression node.
 * Returns a newly allocated qualified type string.
 */
char *qualify_type_node(TSNode node, const char *source_code, QualifyContext *ctx);

/**
 * Qualify and canonicalize a type node in a single pass.
 * Returns a newly allocated string with the canonical, qualified type.
 * This is the preferred function for docs generation.
 */
char *qualify_and_canonicalize_type_node(TSNode node, const char *source_code, QualifyContext *ctx);

/**
 * Qualify a type string (for cases where we don't have the AST node).
 * Returns a newly allocated qualified type string.
 */
char *qualify_type_string(const char *type_str, QualifyContext *ctx);

/* ============================================================================
 * Import resolution helpers
 * ========================================================================== */

/**
 * Look up the module for an unqualified type name.
 * Returns NULL if not found in imports.
 */
const char *qualify_lookup_import(QualifyContext *ctx, const char *type_name);

/**
 * Look up the full module name for an alias.
 * If ambiguous, uses dep_cache to resolve based on referenced_type.
 * Returns NULL if alias not found.
 */
const char *qualify_lookup_alias(QualifyContext *ctx, const char *alias,
                                  const char *referenced_type);

/**
 * Check if a type name is defined locally in this module.
 */
bool qualify_is_local_type(QualifyContext *ctx, const char *type_name);

/**
 * Check if a module is directly imported (available for qualified access).
 */
bool qualify_is_direct_import(QualifyContext *ctx, const char *module_name);

/* ============================================================================
 * Implicit imports
 * ========================================================================== */

/**
 * Apply Elm's implicit imports to a qualification context.
 * Called automatically by qualify_context_create.
 *
 * Elm implicitly imports:
 *   import Basics exposing (..)
 *   import List exposing (List, (::))
 *   import Maybe exposing (Maybe(..))
 *   import Result exposing (Result(..))
 *   import String exposing (String)
 *   import Char exposing (Char)
 *   import Tuple
 *   import Debug
 *   import Platform exposing (Program)
 *   import Platform.Cmd as Cmd exposing (Cmd)
 *   import Platform.Sub as Sub exposing (Sub)
 */
void qualify_apply_implicit_imports(QualifyContext *ctx);

#endif /* AST_QUALIFY_H */
