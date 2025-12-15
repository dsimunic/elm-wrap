/**
 * qualify.c - Type name qualification implementation
 *
 * Qualifies type names in skeleton AST according to import declarations
 * and Elm's implicit imports. Also provides combined qualification and
 * canonicalization for the docs pipeline.
 */

/* Include type_maps.h first so its types are defined before qualify.h */
#include "../commands/publish/docs/type_maps.h"

#include "qualify.h"
#include "util.h"
#include "../alloc.h"
#include "../constants.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

/**
 * Check if a node type is a comment (line_comment or block_comment).
 * Comments should be skipped during type traversal to avoid spurious spacing.
 */
static inline bool is_comment_node(const char *node_type) {
    return strcmp(node_type, "line_comment") == 0 ||
           strcmp(node_type, "block_comment") == 0;
}

/* ============================================================================
 * Context lifecycle
 * ========================================================================== */

QualifyContext *qualify_context_create(SkeletonModule *skeleton, DependencyCache *dep_cache) {
    QualifyContext *ctx = arena_calloc(1, sizeof(QualifyContext));

    ctx->current_module = arena_strdup(skeleton->module_name ? skeleton->module_name : "");
    ctx->dep_cache = dep_cache;

    /* Initialize arrays */
    ctx->imports_capacity = 64;
    ctx->imports = arena_malloc(ctx->imports_capacity * sizeof(QualifyImportEntry));

    ctx->aliases_capacity = 16;
    ctx->aliases = arena_malloc(ctx->aliases_capacity * sizeof(QualifyAliasEntry));

    ctx->direct_modules_capacity = 32;
    ctx->direct_modules = arena_malloc(ctx->direct_modules_capacity * sizeof(char*));

    /* Copy local types from skeleton */
    ctx->local_types = skeleton->local_types;
    ctx->local_types_count = skeleton->local_types_count;

    /* Apply implicit imports first */
    qualify_apply_implicit_imports(ctx);

    /* Process skeleton's explicit imports */
    for (int i = 0; i < skeleton->imports_count; i++) {
        SkeletonImport *imp = &skeleton->imports[i];

        /* Add to direct modules (for qualified access) */
        if (!imp->alias) {
            /* Module is directly available (not aliased) */
            if (ctx->direct_modules_count >= ctx->direct_modules_capacity) {
                ctx->direct_modules_capacity *= 2;
                ctx->direct_modules = arena_realloc(ctx->direct_modules,
                    ctx->direct_modules_capacity * sizeof(char*));
            }
            ctx->direct_modules[ctx->direct_modules_count++] = arena_strdup(imp->module_name);
        } else {
            /* Add alias mapping */
            /* TODO: Handle existing alias with multiple modules */
            if (ctx->aliases_count >= ctx->aliases_capacity) {
                ctx->aliases_capacity *= 2;
                ctx->aliases = arena_realloc(ctx->aliases,
                    ctx->aliases_capacity * sizeof(QualifyAliasEntry));
            }
            QualifyAliasEntry *entry = &ctx->aliases[ctx->aliases_count++];
            entry->alias = arena_strdup(imp->alias);
            entry->full_modules_capacity = 4;
            entry->full_modules = arena_malloc(entry->full_modules_capacity * sizeof(char*));
            entry->full_modules[0] = arena_strdup(imp->module_name);
            entry->full_modules_count = 1;
        }

        /* Process exposing clause */
        if (imp->expose_all) {
            /* TODO: Need to fetch all exports from module via dep_cache */
        } else {
            /* Add exposed types */
            for (int j = 0; j < imp->exposed_types_count; j++) {
                if (ctx->imports_count >= ctx->imports_capacity) {
                    ctx->imports_capacity *= 2;
                    ctx->imports = arena_realloc(ctx->imports,
                        ctx->imports_capacity * sizeof(QualifyImportEntry));
                }
                QualifyImportEntry *entry = &ctx->imports[ctx->imports_count++];
                entry->type_name = arena_strdup(imp->exposed_types[j]);
                entry->module_name = arena_strdup(imp->module_name);
            }
            for (int j = 0; j < imp->exposed_types_with_constructors_count; j++) {
                if (ctx->imports_count >= ctx->imports_capacity) {
                    ctx->imports_capacity *= 2;
                    ctx->imports = arena_realloc(ctx->imports,
                        ctx->imports_capacity * sizeof(QualifyImportEntry));
                }
                QualifyImportEntry *entry = &ctx->imports[ctx->imports_count++];
                entry->type_name = arena_strdup(imp->exposed_types_with_constructors[j]);
                entry->module_name = arena_strdup(imp->module_name);
            }
        }
    }

    return ctx;
}

QualifyContext *qualify_context_create_from_maps(const char *module_name,
                                                  ImportMap *import_map,
                                                  ModuleAliasMap *alias_map,
                                                  DirectModuleImports *direct_imports,
                                                  char **local_types, int local_types_count,
                                                  DependencyCache *dep_cache) {
    QualifyContext *ctx = arena_calloc(1, sizeof(QualifyContext));

    ctx->current_module = arena_strdup(module_name ? module_name : "");
    ctx->dep_cache = dep_cache;

    /* Initialize arrays */
    ctx->imports_capacity = 64;
    ctx->imports = arena_malloc(ctx->imports_capacity * sizeof(QualifyImportEntry));

    ctx->aliases_capacity = 16;
    ctx->aliases = arena_malloc(ctx->aliases_capacity * sizeof(QualifyAliasEntry));

    ctx->direct_modules_capacity = 32;
    ctx->direct_modules = arena_malloc(ctx->direct_modules_capacity * sizeof(char*));

    /* Borrow local types (caller keeps ownership) */
    ctx->local_types = local_types;
    ctx->local_types_count = local_types_count;

    /* Apply implicit imports first */
    qualify_apply_implicit_imports(ctx);

    /* Copy from import_map */
    if (import_map) {
        for (int i = 0; i < import_map->imports_count; i++) {
            if (ctx->imports_count >= ctx->imports_capacity) {
                ctx->imports_capacity *= 2;
                ctx->imports = arena_realloc(ctx->imports, ctx->imports_capacity * sizeof(QualifyImportEntry));
            }
            QualifyImportEntry *entry = &ctx->imports[ctx->imports_count++];
            entry->type_name = arena_strdup(import_map->imports[i].type_name);
            entry->module_name = arena_strdup(import_map->imports[i].module_name);
        }
    }

    /* Copy from alias_map */
    if (alias_map) {
        for (int i = 0; i < alias_map->aliases_count; i++) {
            ModuleAlias *src = &alias_map->aliases[i];
            if (ctx->aliases_count >= ctx->aliases_capacity) {
                ctx->aliases_capacity *= 2;
                ctx->aliases = arena_realloc(ctx->aliases,
                    ctx->aliases_capacity * sizeof(QualifyAliasEntry));
            }
            QualifyAliasEntry *entry = &ctx->aliases[ctx->aliases_count++];
            entry->alias = arena_strdup(src->alias);
            entry->full_modules_capacity = src->full_modules_count > 0 ? src->full_modules_count : 1;
            entry->full_modules = arena_malloc(entry->full_modules_capacity * sizeof(char*));
            entry->full_modules_count = src->full_modules_count;
            for (int j = 0; j < src->full_modules_count; j++) {
                entry->full_modules[j] = arena_strdup(src->full_modules[j]);
            }
        }
    }

    /* Copy from direct_imports */
    if (direct_imports) {
        for (int i = 0; i < direct_imports->modules_count; i++) {
            if (ctx->direct_modules_count >= ctx->direct_modules_capacity) {
                ctx->direct_modules_capacity *= 2;
                ctx->direct_modules = arena_realloc(ctx->direct_modules,
                    ctx->direct_modules_capacity * sizeof(char*));
            }
            ctx->direct_modules[ctx->direct_modules_count++] = arena_strdup(direct_imports->modules[i]);
        }
    }

    return ctx;
}

void qualify_context_free(QualifyContext *ctx) {
    if (!ctx) return;

    arena_free(ctx->current_module);

    for (int i = 0; i < ctx->imports_count; i++) {
        arena_free(ctx->imports[i].type_name);
        arena_free(ctx->imports[i].module_name);
    }
    arena_free(ctx->imports);

    for (int i = 0; i < ctx->aliases_count; i++) {
        arena_free(ctx->aliases[i].alias);
        for (int j = 0; j < ctx->aliases[i].full_modules_count; j++) {
            arena_free(ctx->aliases[i].full_modules[j]);
        }
        arena_free(ctx->aliases[i].full_modules);
    }
    arena_free(ctx->aliases);

    for (int i = 0; i < ctx->direct_modules_count; i++) {
        arena_free(ctx->direct_modules[i]);
    }
    arena_free(ctx->direct_modules);

    /* Note: local_types are owned by skeleton/caller, not freed here */

    arena_free(ctx);
}

/* ============================================================================
 * Implicit imports
 * ========================================================================== */

/* Helper to add an import entry */
static void add_import_entry(QualifyContext *ctx, const char *type_name, const char *module_name) {
    if (ctx->imports_count >= ctx->imports_capacity) {
        ctx->imports_capacity *= 2;
        ctx->imports = arena_realloc(ctx->imports, ctx->imports_capacity * sizeof(QualifyImportEntry));
    }
    QualifyImportEntry *entry = &ctx->imports[ctx->imports_count++];
    entry->type_name = arena_strdup(type_name);
    entry->module_name = arena_strdup(module_name);
}

/* Helper to add alias entry */
static void add_alias_entry(QualifyContext *ctx, const char *alias, const char *module_name) {
    /* Check if alias already exists */
    for (int i = 0; i < ctx->aliases_count; i++) {
        if (strcmp(ctx->aliases[i].alias, alias) == 0) {
            /* Add to existing alias's modules */
            QualifyAliasEntry *entry = &ctx->aliases[i];
            if (entry->full_modules_count >= entry->full_modules_capacity) {
                entry->full_modules_capacity *= 2;
                entry->full_modules = arena_realloc(entry->full_modules,
                    entry->full_modules_capacity * sizeof(char*));
            }
            entry->full_modules[entry->full_modules_count++] = arena_strdup(module_name);
            return;
        }
    }

    /* New alias */
    if (ctx->aliases_count >= ctx->aliases_capacity) {
        ctx->aliases_capacity *= 2;
        ctx->aliases = arena_realloc(ctx->aliases, ctx->aliases_capacity * sizeof(QualifyAliasEntry));
    }
    QualifyAliasEntry *entry = &ctx->aliases[ctx->aliases_count++];
    entry->alias = arena_strdup(alias);
    entry->full_modules_capacity = 4;
    entry->full_modules = arena_malloc(entry->full_modules_capacity * sizeof(char*));
    entry->full_modules[0] = arena_strdup(module_name);
    entry->full_modules_count = 1;
}

/* Helper to add direct import */
static void add_direct_module(QualifyContext *ctx, const char *module_name) {
    /* Check if already present */
    for (int i = 0; i < ctx->direct_modules_count; i++) {
        if (strcmp(ctx->direct_modules[i], module_name) == 0) {
            return;
        }
    }

    if (ctx->direct_modules_count >= ctx->direct_modules_capacity) {
        ctx->direct_modules_capacity *= 2;
        ctx->direct_modules = arena_realloc(ctx->direct_modules,
            ctx->direct_modules_capacity * sizeof(char*));
    }
    ctx->direct_modules[ctx->direct_modules_count++] = arena_strdup(module_name);
}

void qualify_apply_implicit_imports(QualifyContext *ctx) {
    /*
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

    /* Basics exposing (..) - add common types */
    /* Note: String and Char come from their own modules, not Basics */
    const char *basics_types[] = {
        "Int", "Float", "Bool", "Order", "Never"
    };
    for (size_t i = 0; i < sizeof(basics_types) / sizeof(basics_types[0]); i++) {
        add_import_entry(ctx, basics_types[i], "Basics");
    }
    /* Also add Bool constructors */
    add_import_entry(ctx, "True", "Basics");
    add_import_entry(ctx, "False", "Basics");
    /* And Order constructors */
    add_import_entry(ctx, "LT", "Basics");
    add_import_entry(ctx, "EQ", "Basics");
    add_import_entry(ctx, "GT", "Basics");
    add_direct_module(ctx, "Basics");

    /* List exposing (List) */
    add_import_entry(ctx, "List", "List");
    add_direct_module(ctx, "List");

    /* Maybe exposing (Maybe(..)) */
    add_import_entry(ctx, "Maybe", "Maybe");
    add_import_entry(ctx, "Just", "Maybe");
    add_import_entry(ctx, "Nothing", "Maybe");
    add_direct_module(ctx, "Maybe");

    /* Result exposing (Result(..)) */
    add_import_entry(ctx, "Result", "Result");
    add_import_entry(ctx, "Ok", "Result");
    add_import_entry(ctx, "Err", "Result");
    add_direct_module(ctx, "Result");

    /* String exposing (String) */
    add_import_entry(ctx, "String", "String");
    add_direct_module(ctx, "String");

    /* Char exposing (Char) */
    add_import_entry(ctx, "Char", "Char");
    add_direct_module(ctx, "Char");

    /* Tuple - just direct import, no exposing */
    add_direct_module(ctx, "Tuple");

    /* Debug - just direct import, no exposing */
    add_direct_module(ctx, "Debug");

    /* Platform exposing (Program) */
    add_import_entry(ctx, "Program", "Platform");
    add_direct_module(ctx, "Platform");

    /* Platform.Cmd as Cmd exposing (Cmd) */
    add_import_entry(ctx, "Cmd", "Platform.Cmd");
    add_alias_entry(ctx, "Cmd", "Platform.Cmd");

    /* Platform.Sub as Sub exposing (Sub) */
    add_import_entry(ctx, "Sub", "Platform.Sub");
    add_alias_entry(ctx, "Sub", "Platform.Sub");
}

/* ============================================================================
 * Lookup helpers
 * ========================================================================== */

const char *qualify_lookup_import(QualifyContext *ctx, const char *type_name) {
    /* Search backwards to implement "last import wins" semantics:
     * When the same type is exposed from multiple modules, the last import takes precedence.
     * This matches Elm's behavior and the logic in type_maps.c:lookup_import */
    for (int i = ctx->imports_count - 1; i >= 0; i--) {
        if (strcmp(ctx->imports[i].type_name, type_name) == 0) {
            return ctx->imports[i].module_name;
        }
    }
    return NULL;
}

const char *qualify_lookup_alias(QualifyContext *ctx, const char *alias,
                                  const char *referenced_type) {
    for (int i = 0; i < ctx->aliases_count; i++) {
        if (strcmp(ctx->aliases[i].alias, alias) == 0) {
            QualifyAliasEntry *entry = &ctx->aliases[i];
            if (entry->full_modules_count == 1) {
                return entry->full_modules[0];
            }

            /* Multiple modules use this alias - try type-based resolution */
            if (referenced_type && ctx->dep_cache) {
                const char *resolved_module = NULL;
                int matches = 0;

                /* Check which modules export the referenced type */
                for (int j = 0; j < entry->full_modules_count; j++) {
                    CachedModuleExports *exports = dependency_cache_get_exports(ctx->dep_cache, entry->full_modules[j]);
                    if (exports && exports->parsed) {
                        for (int k = 0; k < exports->exported_types_count; k++) {
                            if (strcmp(exports->exported_types[k], referenced_type) == 0) {
                                resolved_module = entry->full_modules[j];
                                matches++;
                                break;
                            }
                        }
                    }
                }

                /* If exactly one module exports it, we've resolved the ambiguity! */
                if (matches == 1) {
                    return resolved_module;
                }
            }

            /* Still ambiguous or no dep_cache - return first module */
            return entry->full_modules[0];
        }
    }
    return NULL;
}

bool qualify_is_local_type(QualifyContext *ctx, const char *type_name) {
    for (int i = 0; i < ctx->local_types_count; i++) {
        if (strcmp(ctx->local_types[i], type_name) == 0) {
            return true;
        }
    }
    return false;
}

bool qualify_is_direct_import(QualifyContext *ctx, const char *module_name) {
    for (int i = 0; i < ctx->direct_modules_count; i++) {
        if (strcmp(ctx->direct_modules[i], module_name) == 0) {
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * Type qualification - AST walking implementation
 * ========================================================================== */

/**
 * Qualify a single type name based on context.
 * Returns a newly allocated string with the qualified name, or a copy of
 * the original if no qualification is needed.
 *
 * Qualification rules:
 * 1. Local types → unchanged
 * 2. Types from exposing clause → Module.Type
 * 3. Already qualified types → expand alias if present (but prefer original module if it exports the type with matching arity)
 * 4. Type variables (lowercase) → unchanged
 *
 * @param type_name The unqualified type name
 * @param module_qualifier The module qualifier (if already qualified), or NULL
 * @param ctx The qualification context
 * @param arity The number of type parameters being applied (for disambiguation)
 */
static char *qualify_single_type_name(const char *type_name, const char *module_qualifier,
                                       QualifyContext *ctx, int arity) {
    /* If there's a module qualifier, we need to expand any alias */
    if (module_qualifier && *module_qualifier) {
        /* Look up the alias */
        const char *full_module = qualify_lookup_alias(ctx, module_qualifier, type_name);
        if (full_module) {
            /* Alias found, but check if there's also a real module with the alias name
             * that is DIRECTLY IMPORTED and exports this type. If so, prefer the real module
             * - BUT only if the arity matches!
             * Example: "import Parser.Advanced as Parser" creates alias Parser -> Parser.Advanced
             * - Parser.DeadEnd context problem (2 params) → Parser.Advanced.DeadEnd (arity 2)
             * - Parser.DeadEnd (0 params) → Parser.DeadEnd (arity 0)
             * This prevents using Parser.DeadEnd when Parser.Advanced.DeadEnd is intended.
             *
             * IMPORTANT: We must check if the module is actually directly imported, not just
             * whether it exists in the package. Otherwise we'll incorrectly prefer a module
             * that happens to exist but isn't imported.
             * Example bug: "import Mapbox.Cmd.Internal as Internal" with module "Internal" existing
             * - Should use: Mapbox.Cmd.Internal.Supported
             * - Bug was using: Internal.Supported (wrong - Internal not imported!) */
            if (qualify_is_direct_import(ctx, module_qualifier) && ctx->dep_cache) {
                CachedModuleExports *alias_as_module = dependency_cache_get_exports(ctx->dep_cache, module_qualifier);
                if (alias_as_module && alias_as_module->parsed) {
                    for (int i = 0; i < alias_as_module->exported_types_count; i++) {
                        if (strcmp(alias_as_module->exported_types[i], type_name) == 0) {
                            /* Found the type in the real module. Check if arity matches. */
                            int type_arity = alias_as_module->exported_types_arity[i];
                            if (type_arity == -1 || type_arity == arity) {
                                /* Arity matches or unknown - prefer the real module name. */
                                size_t len = strlen(module_qualifier) + 1 + strlen(type_name) + 1;
                                char *result = arena_malloc(len);
                                snprintf(result, len, "%s.%s", module_qualifier, type_name);
                                return result;
                            }
                            /* Arity mismatch - fall through to use the alias expansion */
                            break;
                        }
                    }
                }
            }

            /* Use the expanded alias */
            size_t len = strlen(full_module) + 1 + strlen(type_name) + 1;
            char *result = arena_malloc(len);
            snprintf(result, len, "%s.%s", full_module, type_name);
            return result;
        }

        /* Check if it's a direct import (no alias expansion needed) */
        if (qualify_is_direct_import(ctx, module_qualifier)) {
            /* Already qualified with a known module - keep as is */
            size_t len = strlen(module_qualifier) + 1 + strlen(type_name) + 1;
            char *result = arena_malloc(len);
            snprintf(result, len, "%s.%s", module_qualifier, type_name);
            return result;
        }

        /* Unknown qualifier - return as is */
        size_t len = strlen(module_qualifier) + 1 + strlen(type_name) + 1;
        char *result = arena_malloc(len);
        snprintf(result, len, "%s.%s", module_qualifier, type_name);
        return result;
    }

    /* Unqualified type name */

    /* Check if it's a type variable (lowercase) */
    if (type_name[0] >= 'a' && type_name[0] <= 'z') {
        return arena_strdup(type_name);
    }

    /* Check if it's a local type - qualify with current module */
    if (qualify_is_local_type(ctx, type_name)) {
        size_t len = strlen(ctx->current_module) + 1 + strlen(type_name) + 1;
        char *result = arena_malloc(len);
        snprintf(result, len, "%s.%s", ctx->current_module, type_name);
        return result;
    }

    /* Look up in imports */
    const char *module = qualify_lookup_import(ctx, type_name);
    if (module) {
        /* Found in imports - qualify it */
        size_t len = strlen(module) + 1 + strlen(type_name) + 1;
        char *result = arena_malloc(len);
        snprintf(result, len, "%s.%s", module, type_name);
        return result;
    }

    /* Not found - return unqualified (may be a type variable or unknown) */
    return arena_strdup(type_name);
}

/* ============================================================================
 * Helper predicates (formerly in canonicalize.c)
 * ========================================================================== */

static bool type_contains_arrow(TSNode node, const char *source_code) {
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

static bool type_is_tuple(TSNode node, const char *source_code) {
    (void)source_code;
    return strcmp(ts_node_type(node), "tuple_type") == 0;
}

static bool type_is_application(TSNode node, const char *source_code) {
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
 * Combined qualification + canonicalization
 * ========================================================================== */

/**
 * Internal function to qualify AND canonicalize types in a single AST walk.
 * This combines the logic from qualify_type_to_buffer and canonicalize_type_to_buffer
 * to produce the final canonical form with qualified type names.
 *
 * @param node                    The AST node to process
 * @param source_code             The source code
 * @param ctx                     Qualification context
 * @param buffer                  Output buffer
 * @param pos                     Current position in buffer
 * @param max_len                 Buffer capacity
 * @param in_function_arg_position Whether we're in a function argument position
 */
static void qualify_canonicalize_to_buffer(TSNode node, const char *source_code,
                                            QualifyContext *ctx,
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
                /* Skip comments - not part of type structure */
                if (is_comment_node(child_type)) {
                    continue;
                }
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
                    qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, true);
                    ast_buffer_append_char(buffer, pos, max_len, ')');
                } else {
                    qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, is_arg_position);
                }
            }
        }
    } else if (strcmp(node_type, "type_ref") == 0) {
        /* type_ref = upper_case_qid type_arg* */
        uint32_t child_count = ts_node_child_count(node);

        /* Count named children to determine arity (first is type name, rest are args) */
        int arity = 0;
        bool found_type_name = false;

        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (ts_node_is_named(child)) {
                const char *ctype = ts_node_type(child);
                /* Skip comments when counting arity */
                if (is_comment_node(ctype)) {
                    continue;
                }
                if (!found_type_name && strcmp(ctype, "upper_case_qid") == 0) {
                    found_type_name = true;
                } else if (found_type_name) {
                    arity++;
                }
            }
        }

        bool first = true;
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);

            if (ts_node_is_named(child)) {
                /* Skip comments - not part of type structure */
                if (is_comment_node(child_type)) {
                    continue;
                }
                if (!first) {
                    ast_buffer_append_char(buffer, pos, max_len, ' ');
                }

                /* For the type name, qualify it with arity information */
                if (strcmp(child_type, "upper_case_qid") == 0 && first) {
                    char *text = ast_get_node_text(child, source_code);
                    char *last_dot = strrchr(text, '.');
                    if (last_dot) {
                        /* Already qualified: Module.Type or Alias.Type */
                        *last_dot = '\0';
                        const char *module_part = text;
                        const char *type_part = last_dot + 1;
                        char *qualified = qualify_single_type_name(type_part, module_part, ctx, arity);
                        ast_buffer_append(buffer, pos, max_len, qualified);
                        arena_free(qualified);
                    } else {
                        /* Unqualified type name */
                        char *qualified = qualify_single_type_name(text, NULL, ctx, arity);
                        ast_buffer_append(buffer, pos, max_len, qualified);
                        arena_free(qualified);
                    }
                    arena_free(text);
                } else {
                    /* Type argument - check if it needs parentheses */
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
                        qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, false);
                        ast_buffer_append_char(buffer, pos, max_len, ')');
                    } else {
                        qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, false);
                    }
                }
                first = false;
            }
        }
    } else if (strcmp(node_type, "upper_case_qid") == 0) {
        /* Standalone qualified/unqualified type identifier (not in type_ref context, arity = 0) */
        char *text = ast_get_node_text(node, source_code);

        /* Find the last dot to split module.Type */
        char *last_dot = strrchr(text, '.');
        if (last_dot) {
            /* Already qualified: Module.Type or Alias.Type */
            *last_dot = '\0';
            const char *module_part = text;
            const char *type_part = last_dot + 1;

            char *qualified = qualify_single_type_name(type_part, module_part, ctx, 0);
            ast_buffer_append(buffer, pos, max_len, qualified);
            arena_free(qualified);
        } else {
            /* Unqualified type name */
            char *qualified = qualify_single_type_name(text, NULL, ctx, 0);
            ast_buffer_append(buffer, pos, max_len, qualified);
            arena_free(qualified);
        }

        arena_free(text);
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
                    qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, false);
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
                qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, false);
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
                    qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len, false);
                    first = false;
                }
            }

            ast_buffer_append(buffer, pos, max_len, " )");
        }
    } else if (strcmp(node_type, "unit_expr") == 0) {
        /* Unit type () */
        ast_buffer_append(buffer, pos, max_len, "()");
    } else {
        /* Handle parenthesized expressions and other nodes */
        uint32_t child_count = ts_node_child_count(node);
        bool has_paren = false;
        TSNode inner_node = node;

        /* Check for parentheses */
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (!ts_node_is_named(child)) {
                uint32_t start = ts_node_start_byte(child);
                uint32_t end = ts_node_end_byte(child);
                if (end - start == 1 && source_code[start] == '(') {
                    has_paren = true;
                }
            } else {
                const char *child_type = ts_node_type(child);
                if (strcmp(child_type, "type_expression") == 0 ||
                    strcmp(child_type, "type_ref") == 0) {
                    inner_node = child;
                }
            }
        }

        if (has_paren && !ts_node_eq(inner_node, node)) {
            /* Check for unit type */
            uint32_t inner_start = ts_node_start_byte(inner_node);
            uint32_t inner_end = ts_node_end_byte(inner_node);
            if (inner_end == inner_start) {
                /* Unit type */
                ast_buffer_append(buffer, pos, max_len, "()");
            } else {
                /* Parenthesized type - check if parens are needed */
                bool has_arrow = type_contains_arrow(inner_node, source_code);
                bool is_tuple = type_is_tuple(inner_node, source_code);
                bool is_application = type_is_application(inner_node, source_code);

                /* Parens needed for:
                 * - Tuples: always preserved (they define the tuple)
                 * - Function types in arg position: (a -> b) -> c
                 * - Type applications in arg position within type_ref: Foo (Maybe a)
                 */
                bool needs_parens = is_tuple ||
                                    (has_arrow && in_function_arg_position) ||
                                    is_application;

                if (needs_parens) {
                    ast_buffer_append_char(buffer, pos, max_len, '(');
                    qualify_canonicalize_to_buffer(inner_node, source_code, ctx, buffer, pos, max_len, false);
                    ast_buffer_append_char(buffer, pos, max_len, ')');
                } else {
                    qualify_canonicalize_to_buffer(inner_node, source_code, ctx, buffer, pos, max_len,
                                                    in_function_arg_position);
                }
            }
        } else {
            /* Recurse into children */
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                if (ts_node_is_named(child)) {
                    const char *ctype = ts_node_type(child);
                    /* Skip comments - not part of type structure */
                    if (is_comment_node(ctype)) {
                        continue;
                    }
                    qualify_canonicalize_to_buffer(child, source_code, ctx, buffer, pos, max_len,
                                                    in_function_arg_position);
                }
            }
        }
    }
}

/**
 * Qualify and canonicalize a type node in a single pass.
 * Returns a newly allocated string with the canonical, qualified type.
 */
char *qualify_and_canonicalize_type_node(TSNode node, const char *source_code, QualifyContext *ctx) {
    if (ts_node_is_null(node)) {
        return arena_strdup("");
    }

    size_t max_len = MAX_LARGE_BUFFER_LENGTH;  /* 64KB - large records can exceed 4KB */
    char *buffer = arena_malloc(max_len);
    size_t pos = 0;
    buffer[0] = '\0';

    qualify_canonicalize_to_buffer(node, source_code, ctx, buffer, &pos, max_len, false);

    return buffer;
}

/**
 * Internal function to qualify types and write to a buffer.
 * Similar to canonicalize_type_to_buffer but handles qualification.
 */
static void qualify_type_to_buffer(TSNode node, const char *source_code,
                                    QualifyContext *ctx,
                                    char *buffer, size_t *pos, size_t max_len) {
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
                /* Skip comments - not part of type structure */
                if (is_comment_node(child_type)) {
                    continue;
                }
                qualify_type_to_buffer(child, source_code, ctx, buffer, pos, max_len);
            }
        }
    } else if (strcmp(node_type, "type_ref") == 0) {
        /* type_ref = upper_case_qid type_arg* */
        uint32_t child_count = ts_node_child_count(node);

        /* Count type arguments for arity */
        int arity = 0;
        bool found_type_name = false;
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (ts_node_is_named(child)) {
                const char *ctype = ts_node_type(child);
                /* Skip comments when counting arity */
                if (is_comment_node(ctype)) {
                    continue;
                }
                if (!found_type_name && strcmp(ctype, "upper_case_qid") == 0) {
                    found_type_name = true;
                } else if (found_type_name) {
                    arity++;
                }
            }
        }

        bool first = true;
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);

            if (ts_node_is_named(child)) {
                /* Skip comments - not part of type structure */
                if (is_comment_node(child_type)) {
                    continue;
                }
                if (!first) {
                    ast_buffer_append_char(buffer, pos, max_len, ' ');
                }

                /* For the type name, qualify it with arity */
                if (strcmp(child_type, "upper_case_qid") == 0 && first) {
                    char *text = ast_get_node_text(child, source_code);
                    char *last_dot = strrchr(text, '.');
                    if (last_dot) {
                        *last_dot = '\0';
                        const char *module_part = text;
                        const char *type_part = last_dot + 1;
                        char *qualified = qualify_single_type_name(type_part, module_part, ctx, arity);
                        ast_buffer_append(buffer, pos, max_len, qualified);
                        arena_free(qualified);
                    } else {
                        char *qualified = qualify_single_type_name(text, NULL, ctx, arity);
                        ast_buffer_append(buffer, pos, max_len, qualified);
                        arena_free(qualified);
                    }
                    arena_free(text);
                } else {
                    qualify_type_to_buffer(child, source_code, ctx, buffer, pos, max_len);
                }
                first = false;
            }
        }
    } else if (strcmp(node_type, "upper_case_qid") == 0) {
        /* Standalone qualified/unqualified type identifier (arity = 0) */
        char *text = ast_get_node_text(node, source_code);

        /* Find the last dot to split module.Type */
        char *last_dot = strrchr(text, '.');
        if (last_dot) {
            /* Already qualified: Module.Type or Alias.Type */
            *last_dot = '\0';
            const char *module_part = text;
            const char *type_part = last_dot + 1;

            char *qualified = qualify_single_type_name(type_part, module_part, ctx, 0);
            ast_buffer_append(buffer, pos, max_len, qualified);
            arena_free(qualified);
        } else {
            /* Unqualified type name */
            char *qualified = qualify_single_type_name(text, NULL, ctx, 0);
            ast_buffer_append(buffer, pos, max_len, qualified);
            arena_free(qualified);
        }

        arena_free(text);
    } else if (strcmp(node_type, "type_variable") == 0 ||
               strcmp(node_type, "lower_case_identifier") == 0) {
        /* Type variable - output as-is */
        ast_buffer_append_node_text(buffer, pos, max_len, node, source_code);
    } else if (strcmp(node_type, "record_type") == 0) {
        /* Record type { field : type, ... } */
        ast_buffer_append(buffer, pos, max_len, "{ ");

        uint32_t child_count = ts_node_child_count(node);
        bool first_field = true;

        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char *child_type = ts_node_type(child);

            if (strcmp(child_type, "field_type") == 0) {
                if (!first_field) {
                    ast_buffer_append(buffer, pos, max_len, ", ");
                }
                qualify_type_to_buffer(child, source_code, ctx, buffer, pos, max_len);
                first_field = false;
            } else if (strcmp(child_type, "record_base_identifier") == 0) {
                /* Extensible record: { a | field : type } */
                ast_buffer_append_node_text(buffer, pos, max_len, child, source_code);
                ast_buffer_append(buffer, pos, max_len, " | ");
            }
        }

        ast_buffer_append(buffer, pos, max_len, " }");
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
                qualify_type_to_buffer(child, source_code, ctx, buffer, pos, max_len);
            }
        }
    } else if (strcmp(node_type, "tuple_type") == 0) {
        /* Tuple ( type, type, ... ) */
        ast_buffer_append(buffer, pos, max_len, "( ");

        uint32_t child_count = ts_node_child_count(node);
        bool first = true;

        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);

            if (ts_node_is_named(child) &&
                strcmp(ts_node_type(child), "type_expression") == 0) {
                if (!first) {
                    ast_buffer_append(buffer, pos, max_len, ", ");
                }
                qualify_type_to_buffer(child, source_code, ctx, buffer, pos, max_len);
                first = false;
            }
        }

        ast_buffer_append(buffer, pos, max_len, " )");
    } else if (strcmp(node_type, "unit_expr") == 0) {
        /* Unit type () */
        ast_buffer_append(buffer, pos, max_len, "()");
    } else {
        /* Handle parenthesized expressions and other nodes */
        uint32_t child_count = ts_node_child_count(node);
        bool has_paren = false;
        TSNode inner_node = node;

        /* Check for parentheses */
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            if (!ts_node_is_named(child)) {
                uint32_t start = ts_node_start_byte(child);
                uint32_t end = ts_node_end_byte(child);
                if (end - start == 1 && source_code[start] == '(') {
                    has_paren = true;
                }
            } else {
                const char *child_type = ts_node_type(child);
                if (strcmp(child_type, "type_expression") == 0 ||
                    strcmp(child_type, "type_ref") == 0) {
                    inner_node = child;
                }
            }
        }

        if (has_paren && !ts_node_eq(inner_node, node)) {
            /* Check for unit type */
            uint32_t inner_start = ts_node_start_byte(inner_node);
            uint32_t inner_end = ts_node_end_byte(inner_node);
            if (inner_end == inner_start) {
                /* Unit type */
                ast_buffer_append(buffer, pos, max_len, "()");
            } else {
                /* Parenthesized type - preserve parens for now, let canonicalize handle removal */
                ast_buffer_append_char(buffer, pos, max_len, '(');
                qualify_type_to_buffer(inner_node, source_code, ctx, buffer, pos, max_len);
                ast_buffer_append_char(buffer, pos, max_len, ')');
            }
        } else {
            /* Recurse into children */
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                if (ts_node_is_named(child)) {
                    const char *ctype = ts_node_type(child);
                    /* Skip comments - not part of type structure */
                    if (is_comment_node(ctype)) {
                        continue;
                    }
                    qualify_type_to_buffer(child, source_code, ctx, buffer, pos, max_len);
                }
            }
        }
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

char *qualify_type_node(TSNode node, const char *source_code, QualifyContext *ctx) {
    if (ts_node_is_null(node)) {
        return arena_strdup("");
    }

    size_t max_len = MAX_LARGE_BUFFER_LENGTH;  /* 64KB - large records can exceed 4KB */
    char *buffer = arena_malloc(max_len);
    size_t pos = 0;
    buffer[0] = '\0';

    qualify_type_to_buffer(node, source_code, ctx, buffer, &pos, max_len);

    return buffer;
}

void qualify_skeleton_types(SkeletonModule *skeleton, QualifyContext *ctx) {
    if (!skeleton || !ctx) return;

    const char *source_code = skeleton->source_code;

    /* Qualify type annotations */
    for (int i = 0; i < skeleton->type_annotations_count; i++) {
        SkeletonTypeAnnotation *ann = &skeleton->type_annotations[i];
        if (!ts_node_is_null(ann->type_node)) {
            ann->qualified_type = qualify_type_node(ann->type_node, source_code, ctx);
        }
    }

    /* Qualify type aliases */
    for (int i = 0; i < skeleton->type_aliases_count; i++) {
        SkeletonTypeAlias *alias = &skeleton->type_aliases[i];
        if (!ts_node_is_null(alias->type_node)) {
            alias->qualified_type = qualify_type_node(alias->type_node, source_code, ctx);
        }
    }

    /* Qualify union type constructors */
    for (int i = 0; i < skeleton->union_types_count; i++) {
        SkeletonUnionType *union_type = &skeleton->union_types[i];
        for (int j = 0; j < union_type->constructors_count; j++) {
            SkeletonUnionConstructor *ctor = &union_type->constructors[j];

            /* Allocate arrays for qualified args */
            if (ctor->arg_nodes_count > 0) {
                ctor->qualified_args = arena_malloc(ctor->arg_nodes_count * sizeof(char*));
                for (int k = 0; k < ctor->arg_nodes_count; k++) {
                    if (!ts_node_is_null(ctor->arg_nodes[k])) {
                        ctor->qualified_args[k] = qualify_type_node(ctor->arg_nodes[k], source_code, ctx);
                    } else {
                        ctor->qualified_args[k] = arena_strdup("");
                    }
                }
            }
        }
    }
}

void canonicalize_skeleton_types(SkeletonModule *skeleton, QualifyContext *ctx) {
    if (!skeleton || !ctx) return;

    const char *source_code = skeleton->source_code;

    /* Canonicalize type annotations using combined qualify+canonicalize */
    for (int i = 0; i < skeleton->type_annotations_count; i++) {
        SkeletonTypeAnnotation *ann = &skeleton->type_annotations[i];
        if (!ts_node_is_null(ann->type_node)) {
            ann->canonical_type = qualify_and_canonicalize_type_node(ann->type_node, source_code, ctx);
        }
    }

    /* Canonicalize type aliases */
    for (int i = 0; i < skeleton->type_aliases_count; i++) {
        SkeletonTypeAlias *alias = &skeleton->type_aliases[i];
        if (!ts_node_is_null(alias->type_node)) {
            alias->canonical_type = qualify_and_canonicalize_type_node(alias->type_node, source_code, ctx);
        }
    }

    /* Canonicalize union type constructors */
    for (int i = 0; i < skeleton->union_types_count; i++) {
        SkeletonUnionType *union_type = &skeleton->union_types[i];
        for (int j = 0; j < union_type->constructors_count; j++) {
            SkeletonUnionConstructor *ctor = &union_type->constructors[j];

            /* Allocate arrays for canonical args */
            if (ctor->arg_nodes_count > 0) {
                ctor->canonical_args = arena_malloc(ctor->arg_nodes_count * sizeof(char*));
                for (int k = 0; k < ctor->arg_nodes_count; k++) {
                    if (!ts_node_is_null(ctor->arg_nodes[k])) {
                        ctor->canonical_args[k] = qualify_and_canonicalize_type_node(
                            ctor->arg_nodes[k], source_code, ctx);
                    } else {
                        ctor->canonical_args[k] = arena_strdup("");
                    }
                }
            }
        }
    }
}

char *qualify_type_string(const char *type_str, QualifyContext *ctx) {
    /* Fallback for string-based qualification when AST is not available */
    /* This is a simplified implementation that handles common cases */
    if (!type_str || !ctx) {
        return arena_strdup(type_str ? type_str : "");
    }

    size_t len = strlen(type_str);
    size_t max_len = len * 3 + 256;  /* Allow for expansion */
    char *result = arena_malloc(max_len);
    size_t pos = 0;

    const char *p = type_str;
    while (*p && pos < max_len - 64) {
        /* Check for uppercase identifier (type name) */
        if (*p >= 'A' && *p <= 'Z') {
            const char *start = p;

            /* Collect the full qualified identifier (including dots) */
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '_' || *p == '.') {
                p++;
            }

            size_t id_len = p - start;
            char *identifier = arena_malloc(id_len + 1);
            memcpy(identifier, start, id_len);
            identifier[id_len] = '\0';

            /* Split by last dot */
            char *last_dot = strrchr(identifier, '.');
            char *qualified;

            if (last_dot) {
                *last_dot = '\0';
                /* Use arity 0 (unknown) for string-based qualification */
                qualified = qualify_single_type_name(last_dot + 1, identifier, ctx, 0);
            } else {
                qualified = qualify_single_type_name(identifier, NULL, ctx, 0);
            }

            size_t qlen = strlen(qualified);
            memcpy(result + pos, qualified, qlen);
            pos += qlen;

            arena_free(qualified);
            arena_free(identifier);
        } else {
            result[pos++] = *p++;
        }
    }

    result[pos] = '\0';
    return result;
}
