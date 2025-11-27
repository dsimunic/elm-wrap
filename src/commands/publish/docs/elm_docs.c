#include "elm_docs.h"
#include "dependency_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>
#include "../../../alloc.h"

/* External tree-sitter language function */
extern TSLanguage *tree_sitter_elm(void);

/* Helper function to read file contents and normalize line endings to \n */
static char *read_file(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        fprintf(stderr, "Error: Could not open file %s\n", filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = arena_malloc(file_size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(content, 1, file_size, file);
    content[read_size] = '\0';
    fclose(file);

    /* Normalize line endings: convert \r\n and \r to \n */
    char *normalized = arena_malloc(read_size + 1);
    size_t write_pos = 0;
    for (size_t i = 0; i < read_size; i++) {
        if (content[i] == '\r') {
            /* Skip \r - if followed by \n, we'll get it next iteration */
            if (i + 1 < read_size && content[i + 1] == '\n') {
                continue;  /* \r\n -> skip the \r, keep the \n */
            } else {
                normalized[write_pos++] = '\n';  /* standalone \r -> convert to \n */
            }
        } else {
            normalized[write_pos++] = content[i];
        }
    }
    normalized[write_pos] = '\0';

    arena_free(content);
    return normalized;
}

/* Helper function to get node text */
static char *get_node_text(TSNode node, const char *source_code) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    uint32_t length = end - start;

    char *text = arena_malloc(length + 1);
    if (!text) return NULL;

    memcpy(text, source_code + start, length);
    text[length] = '\0';
    return text;
}

/* Import tracking */
typedef struct {
    char *type_name;       /* The type name as used in this module */
    char *module_name;     /* The module it comes from */
} TypeImport;

typedef struct {
    TypeImport *imports;
    int imports_count;
    int imports_capacity;
} ImportMap;

/* Direct module imports (not via exposing, just module availability) */
typedef struct {
    char **modules;
    int modules_count;
    int modules_capacity;
} DirectModuleImports;

static void init_direct_imports(DirectModuleImports *imports) {
    imports->modules = arena_malloc(16 * sizeof(char*));
    imports->modules_count = 0;
    imports->modules_capacity = 16;
}

static void add_direct_import(DirectModuleImports *imports, const char *module_name) {
    /* Check if already exists */
    for (int i = 0; i < imports->modules_count; i++) {
        if (strcmp(imports->modules[i], module_name) == 0) {
            return;  /* Already added */
        }
    }

    if (imports->modules_count >= imports->modules_capacity) {
        imports->modules_capacity *= 2;
        imports->modules = arena_realloc(imports->modules, imports->modules_capacity * sizeof(char*));
    }
    imports->modules[imports->modules_count++] = arena_strdup(module_name);
}

/* Remove a module from direct imports (used when alias overwrites a direct import) */
static void remove_direct_import(DirectModuleImports *imports, const char *module_name) {
    for (int i = 0; i < imports->modules_count; i++) {
        if (strcmp(imports->modules[i], module_name) == 0) {
            arena_free(imports->modules[i]);
            /* Shift remaining entries */
            for (int j = i; j < imports->modules_count - 1; j++) {
                imports->modules[j] = imports->modules[j + 1];
            }
            imports->modules_count--;
            return;
        }
    }
}

static bool is_directly_imported(DirectModuleImports *imports, const char *module_name) {
    for (int i = 0; i < imports->modules_count; i++) {
        if (strcmp(imports->modules[i], module_name) == 0) {
            return true;
        }
    }
    return false;
}

static void free_direct_imports(DirectModuleImports *imports) {
    for (int i = 0; i < imports->modules_count; i++) {
        arena_free(imports->modules[i]);
    }
    arena_free(imports->modules);
}

/* Module alias tracking (for 'import Foo as F') */
typedef struct {
    char *alias;           /* The alias used in this module (e.g., "D") */
    char *full_module;     /* The full module name (e.g., "Json.Decode") */
    bool is_ambiguous;     /* True if multiple different modules use this alias */
    char *ambiguous_with;  /* If ambiguous, the other module name (for error reporting) */
} ModuleAlias;

typedef struct {
    ModuleAlias *aliases;
    int aliases_count;
    int aliases_capacity;
} ModuleAliasMap;

/* Type alias tracking for expansion */
typedef struct {
    char *type_name;       /* The type name (e.g., "Decoder") */
    char **type_vars;      /* Type variables (e.g., ["a"]) */
    int type_vars_count;
    char *expansion;       /* The expansion (e.g., "Context -> Edn -> Result String a") */
} TypeAliasExpansion;

typedef struct {
    TypeAliasExpansion *aliases;
    int aliases_count;
    int aliases_capacity;
} TypeAliasMap;

/* Export list tracking */
typedef struct {
    char **exposed_values;
    int exposed_values_count;
    char **exposed_types;
    int exposed_types_count;
    char **exposed_types_with_constructors;  /* Types exposed with (..) */
    int exposed_types_with_constructors_count;
    bool expose_all;
} ExportList;

static void init_import_map(ImportMap *map) {
    map->imports = arena_malloc(32 * sizeof(TypeImport));
    map->imports_count = 0;
    map->imports_capacity = 32;
}

static void add_import(ImportMap *map, const char *type_name, const char *module_name) {
    if (map->imports_count >= map->imports_capacity) {
        map->imports_capacity *= 2;
        map->imports = arena_realloc(map->imports, map->imports_capacity * sizeof(TypeImport));
    }
    map->imports[map->imports_count].type_name = arena_strdup(type_name);
    map->imports[map->imports_count].module_name = arena_strdup(module_name);
    map->imports_count++;
}

static const char *lookup_import(ImportMap *map, const char *type_name) {
    /* Search backwards to implement "last import wins" semantics:
     * When the same type is exposed from multiple modules, the last import takes precedence */
    for (int i = map->imports_count - 1; i >= 0; i--) {
        if (strcmp(map->imports[i].type_name, type_name) == 0) {
            return map->imports[i].module_name;
        }
    }
    return NULL;
}

static void free_import_map(ImportMap *map) {
    for (int i = 0; i < map->imports_count; i++) {
        arena_free(map->imports[i].type_name);
        arena_free(map->imports[i].module_name);
    }
    arena_free(map->imports);
}

/* Module alias map functions */
static void init_module_alias_map(ModuleAliasMap *map) {
    map->aliases = arena_malloc(16 * sizeof(ModuleAlias));
    map->aliases_count = 0;
    map->aliases_capacity = 16;
}

static void add_module_alias(ModuleAliasMap *map, const char *alias, const char *full_module) {
    /* Check if this alias already exists */
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].alias, alias) == 0) {
            /* Same module with same alias is fine (not ambiguous) */
            if (strcmp(map->aliases[i].full_module, full_module) == 0) {
                /* Already have this exact mapping, nothing to do */
                return;
            }
            /* Different module with same alias - mark as AMBIGUOUS */
            /* This matches Elm compiler behavior: two different modules */
            /* imported with the same alias causes ambiguity errors on use */
            if (!map->aliases[i].is_ambiguous) {
                map->aliases[i].is_ambiguous = true;
                map->aliases[i].ambiguous_with = arena_strdup(full_module);
            }
            /* Note: we keep the original full_module for error reporting */
            return;
        }
    }

    if (map->aliases_count >= map->aliases_capacity) {
        map->aliases_capacity *= 2;
        map->aliases = arena_realloc(map->aliases, map->aliases_capacity * sizeof(ModuleAlias));
    }
    map->aliases[map->aliases_count].alias = arena_strdup(alias);
    map->aliases[map->aliases_count].full_module = arena_strdup(full_module);
    map->aliases[map->aliases_count].is_ambiguous = false;
    map->aliases[map->aliases_count].ambiguous_with = NULL;
    map->aliases_count++;
}

/* Returns the full module name, or NULL if not found or ambiguous */
/* Sets *is_ambiguous to true if the alias refers to multiple different modules */
/* Helper to check if a module exports a given type name */
static bool module_exports_type(DependencyCache *dep_cache, const char *module_name, const char *type_name) {
    if (!dep_cache || !module_name || !type_name) {
        return false;
    }

    CachedModuleExports *exports = dependency_cache_get_exports(dep_cache, module_name);
    if (!exports || !exports->parsed) {
        return false;
    }

    for (int i = 0; i < exports->exported_types_count; i++) {
        if (strcmp(exports->exported_types[i], type_name) == 0) {
            return true;
        }
    }

    return false;
}

static const char *lookup_module_alias(ModuleAliasMap *map, const char *alias,
                                       const char *referenced_type,
                                       DependencyCache *dep_cache,
                                       bool *is_ambiguous,
                                       const char **ambiguous_module1,
                                       const char **ambiguous_module2) {
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].alias, alias) == 0) {
            if (map->aliases[i].is_ambiguous) {
                /* Try to resolve ambiguous alias by checking which module exports the type */
                if (referenced_type && dep_cache) {
                    const char *mod1 = map->aliases[i].full_module;
                    const char *mod2 = map->aliases[i].ambiguous_with;

                    bool mod1_has = module_exports_type(dep_cache, mod1, referenced_type);
                    bool mod2_has = module_exports_type(dep_cache, mod2, referenced_type);

                    if (mod1_has && !mod2_has) {
                        /* Only mod1 exports it - resolved! */
                        if (is_ambiguous) *is_ambiguous = false;
                        return mod1;
                    } else if (mod2_has && !mod1_has) {
                        /* Only mod2 exports it - resolved! */
                        if (is_ambiguous) *is_ambiguous = false;
                        return mod2;
                    }
                    /* If both export it or neither exports it, fall through to ambiguous handling */
                }

                /* Still ambiguous or couldn't resolve */
                if (is_ambiguous) *is_ambiguous = true;
                if (ambiguous_module1) *ambiguous_module1 = map->aliases[i].full_module;
                if (ambiguous_module2) *ambiguous_module2 = map->aliases[i].ambiguous_with;
                return NULL;  /* Ambiguous - cannot resolve */
            }
            if (is_ambiguous) *is_ambiguous = false;
            return map->aliases[i].full_module;
        }
    }
    if (is_ambiguous) *is_ambiguous = false;
    return NULL;
}

static void free_module_alias_map(ModuleAliasMap *map) {
    for (int i = 0; i < map->aliases_count; i++) {
        arena_free(map->aliases[i].alias);
        arena_free(map->aliases[i].full_module);
        if (map->aliases[i].ambiguous_with) {
            arena_free(map->aliases[i].ambiguous_with);
        }
    }
    arena_free(map->aliases);
}

/* Type alias map functions */
static void init_type_alias_map(TypeAliasMap *map) {
    map->aliases = arena_malloc(16 * sizeof(TypeAliasExpansion));
    map->aliases_count = 0;
    map->aliases_capacity = 16;
}

static void add_type_alias(TypeAliasMap *map, const char *type_name, char **type_vars, int type_vars_count, const char *expansion) {
    if (map->aliases_count >= map->aliases_capacity) {
        map->aliases_capacity *= 2;
        map->aliases = arena_realloc(map->aliases, map->aliases_capacity * sizeof(TypeAliasExpansion));
    }
    map->aliases[map->aliases_count].type_name = arena_strdup(type_name);
    map->aliases[map->aliases_count].type_vars = type_vars;
    map->aliases[map->aliases_count].type_vars_count = type_vars_count;
    map->aliases[map->aliases_count].expansion = arena_strdup(expansion);
    map->aliases_count++;
}

static const TypeAliasExpansion *lookup_type_alias(TypeAliasMap *map, const char *type_name) {
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].type_name, type_name) == 0) {
            return &map->aliases[i];
        }
    }
    return NULL;
}

static void free_type_alias_map(TypeAliasMap *map) {
    for (int i = 0; i < map->aliases_count; i++) {
        arena_free(map->aliases[i].type_name);
        for (int j = 0; j < map->aliases[i].type_vars_count; j++) {
            arena_free(map->aliases[i].type_vars[j]);
        }
        arena_free(map->aliases[i].type_vars);
        arena_free(map->aliases[i].expansion);
    }
    arena_free(map->aliases);
}

static void free_export_list(ExportList *exports) {
    for (int i = 0; i < exports->exposed_values_count; i++) {
        arena_free(exports->exposed_values[i]);
    }
    arena_free(exports->exposed_values);

    for (int i = 0; i < exports->exposed_types_count; i++) {
        arena_free(exports->exposed_types[i]);
    }
    arena_free(exports->exposed_types);

    for (int i = 0; i < exports->exposed_types_with_constructors_count; i++) {
        arena_free(exports->exposed_types_with_constructors[i]);
    }
    arena_free(exports->exposed_types_with_constructors);
}

static bool is_exported_value(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_values_count; i++) {
        if (strcmp(exports->exposed_values[i], name) == 0) return true;
    }
    return false;
}

static bool is_exported_type(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_types_count; i++) {
        if (strcmp(exports->exposed_types[i], name) == 0) return true;
    }
    return false;
}

static bool is_type_exposed_with_constructors(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_types_with_constructors_count; i++) {
        if (strcmp(exports->exposed_types_with_constructors[i], name) == 0) return true;
    }
    return false;
}

/* Helper function to extract module name and exports */
static char *extract_module_info(TSNode root, const char *source_code, ExportList *exports) {
    uint32_t child_count = ts_node_child_count(root);
    char *module_name = NULL;

    /* Initialize export list */
    exports->exposed_values = NULL;
    exports->exposed_values_count = 0;
    exports->exposed_types = NULL;
    exports->exposed_types_count = 0;
    exports->exposed_types_with_constructors = NULL;
    exports->exposed_types_with_constructors_count = 0;
    exports->expose_all = false;

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "module_declaration") == 0) {
            /* Find the upper_case_qid node which contains the module name */
            uint32_t mod_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < mod_child_count; j++) {
                TSNode mod_child = ts_node_child(child, j);
                const char *mod_type = ts_node_type(mod_child);

                if (strcmp(mod_type, "upper_case_qid") == 0) {
                    module_name = get_node_text(mod_child, source_code);
                } else if (strcmp(mod_type, "exposing_list") == 0) {
                    /* Parse exposing list */
                    uint32_t exp_child_count = ts_node_child_count(mod_child);
                    for (uint32_t k = 0; k < exp_child_count; k++) {
                        TSNode exp_child = ts_node_child(mod_child, k);
                        const char *exp_type = ts_node_type(exp_child);

                        if (strcmp(exp_type, "double_dot") == 0) {
                            exports->expose_all = true;
                        } else if (strcmp(exp_type, "exposed_value") == 0) {
                            /* Exposed value/function */
                            char *value_name = get_node_text(exp_child, source_code);
                            exports->exposed_values = arena_realloc(exports->exposed_values,
                                (exports->exposed_values_count + 1) * sizeof(char*));
                            exports->exposed_values[exports->exposed_values_count++] = value_name;
                        } else if (strcmp(exp_type, "exposed_type") == 0) {
                            /* Exposed type */
                            char *type_name = NULL;
                            bool has_constructors = false;

                            uint32_t type_child_count = ts_node_child_count(exp_child);
                            for (uint32_t m = 0; m < type_child_count; m++) {
                                TSNode type_child = ts_node_child(exp_child, m);
                                const char *tc_type = ts_node_type(type_child);

                                if (strcmp(tc_type, "upper_case_identifier") == 0) {
                                    type_name = get_node_text(type_child, source_code);
                                } else if (strcmp(tc_type, "exposed_union_constructors") == 0) {
                                    /* Check if this has (..) */
                                    has_constructors = true;
                                }
                            }

                            if (type_name) {
                                exports->exposed_types = arena_realloc(exports->exposed_types,
                                    (exports->exposed_types_count + 1) * sizeof(char*));
                                exports->exposed_types[exports->exposed_types_count++] = arena_strdup(type_name);

                                if (has_constructors) {
                                    exports->exposed_types_with_constructors = arena_realloc(
                                        exports->exposed_types_with_constructors,
                                        (exports->exposed_types_with_constructors_count + 1) * sizeof(char*));
                                    exports->exposed_types_with_constructors[
                                        exports->exposed_types_with_constructors_count++] = type_name;
                                } else {
                                    arena_free(type_name);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return module_name ? module_name : arena_strdup("Unknown");
}

/* Forward declaration */
static void apply_wellknown_module_exports(ImportMap *import_map, const char *module_name);

/* Helper function to parse imports */
static void extract_imports(TSNode root, const char *source_code, ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports, DependencyCache *dep_cache) {
    uint32_t child_count = ts_node_child_count(root);

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "import_clause") == 0) {
            char *module_name = NULL;
            char *module_alias = NULL;
            bool has_as_clause = false;

            /* First pass: find module name and check for alias */
            uint32_t import_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < import_child_count; j++) {
                TSNode import_child = ts_node_child(child, j);
                const char *import_child_type = ts_node_type(import_child);

                if (strcmp(import_child_type, "upper_case_qid") == 0) {
                    module_name = get_node_text(import_child, source_code);
                } else if (strcmp(import_child_type, "as_clause") == 0) {
                    has_as_clause = true;
                    /* Extract the alias from as_clause */
                    uint32_t as_child_count = ts_node_child_count(import_child);
                    for (uint32_t k = 0; k < as_child_count; k++) {
                        TSNode as_child = ts_node_child(import_child, k);
                        if (strcmp(ts_node_type(as_child), "upper_case_identifier") == 0) {
                            module_alias = get_node_text(as_child, source_code);
                            break;
                        }
                    }
                }
            }

            /* Handle import semantics:
             * - Aliased imports: the alias name shadows any direct import with same name
             * - Direct imports: the module is available by its original name
             * Note: Aliases and direct imports can coexist for the same module */
            if (module_name && direct_imports) {
                if (!has_as_clause) {
                    /* Direct import: module is now available by its original name */
                    add_direct_import(direct_imports, module_name);
                } else {
                    /* Aliased import: alias name shadows any direct import with same name */
                    remove_direct_import(direct_imports, module_alias);
                }
            }

            /* Second pass: process exposing list */
            for (uint32_t j = 0; j < import_child_count; j++) {
                TSNode import_child = ts_node_child(child, j);
                const char *import_child_type = ts_node_type(import_child);

                if (strcmp(import_child_type, "exposing_list") == 0 && module_name) {
                    /* Parse the exposing list */
                    uint32_t exp_child_count = ts_node_child_count(import_child);
                    for (uint32_t k = 0; k < exp_child_count; k++) {
                        TSNode exp_child = ts_node_child(import_child, k);
                        const char *exp_type = ts_node_type(exp_child);

                        if (strcmp(exp_type, "double_dot") == 0) {
                            /* import ModuleName exposing (..) - need to get all exports */
                            bool found_exports = false;
                            if (dep_cache) {
                                CachedModuleExports *exports = dependency_cache_get_exports(dep_cache, module_name);
                                if (exports && exports->parsed && exports->exported_types_count > 0) {
                                    /* Add all exported types to the import map */
                                    for (int t = 0; t < exports->exported_types_count; t++) {
                                        add_import(import_map, exports->exported_types[t], module_name);
                                    }
                                    found_exports = true;
                                }
                            }
                            /* Fallback to well-known module exports if dependency cache failed */
                            if (!found_exports) {
                                apply_wellknown_module_exports(import_map, module_name);
                            }
                        } else if (strcmp(exp_type, "exposed_type") == 0) {
                            /* Extract type name */
                            uint32_t type_child_count = ts_node_child_count(exp_child);
                            for (uint32_t m = 0; m < type_child_count; m++) {
                                TSNode type_child = ts_node_child(exp_child, m);
                                if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                                    char *type_name = get_node_text(type_child, source_code);
                                    add_import(import_map, type_name, module_name);
                                    arena_free(type_name);
                                    break;
                                }
                            }
                        } else if (strcmp(exp_type, "exposed_value") == 0) {
                            /* Check if it's an uppercase identifier (type constructor) */
                            char *value_name = get_node_text(exp_child, source_code);
                            if (value_name && value_name[0] >= 'A' && value_name[0] <= 'Z') {
                                add_import(import_map, value_name, module_name);
                            }
                            arena_free(value_name);
                        }
                    }
                }
            }

            /* Record module alias if present */
            if (module_name && module_alias) {
                add_module_alias(alias_map, module_alias, module_name);
            }

            if (module_name) {
                arena_free(module_name);
            }
            if (module_alias) {
                arena_free(module_alias);
            }
        }
    }
}

/* Placeholder for well-known module exports fallback - currently disabled for testing */
static void apply_wellknown_module_exports(ImportMap *import_map, const char *module_name) {
    (void)import_map;
    (void)module_name;
    /* Fallback disabled - relying on dependency cache to parse module sources */
}

/* Apply Elm's implicit imports
 * Elm implicitly imports the following from elm/core:
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
 * 
 * We set up direct imports and aliases here; actual type exports are resolved
 * via dependency cache when processing explicit imports or via this function
 * for implicitly exposed types.
 */
static void apply_implicit_imports(ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports, DependencyCache *dep_cache) {
    /* Set up direct imports for all implicit modules */
    add_direct_import(direct_imports, "Basics");
    add_direct_import(direct_imports, "List");
    add_direct_import(direct_imports, "Maybe");
    add_direct_import(direct_imports, "Result");
    add_direct_import(direct_imports, "String");
    add_direct_import(direct_imports, "Char");
    add_direct_import(direct_imports, "Tuple");
    add_direct_import(direct_imports, "Debug");
    add_direct_import(direct_imports, "Platform");
    add_direct_import(direct_imports, "Platform.Cmd");
    add_direct_import(direct_imports, "Platform.Sub");
    
    /* Set up module aliases */
    add_module_alias(alias_map, "Cmd", "Platform.Cmd");
    add_module_alias(alias_map, "Sub", "Platform.Sub");
    
    /* Now resolve the exposed types via dependency cache */

    /* Basics exposing (..) */
    /* First add the compiler primitive types that won't be found by scanning */
    add_import(import_map, "Int", "Basics");
    add_import(import_map, "Float", "Basics");
    add_import(import_map, "Bool", "Basics");
    add_import(import_map, "True", "Basics");
    add_import(import_map, "False", "Basics");
    add_import(import_map, "Order", "Basics");
    add_import(import_map, "LT", "Basics");
    add_import(import_map, "EQ", "Basics");
    add_import(import_map, "GT", "Basics");
    add_import(import_map, "Never", "Basics");

    /* Then add any other types found by scanning the Basics module */
    if (dep_cache) {
        CachedModuleExports *basics = dependency_cache_get_exports(dep_cache, "Basics");
        if (basics && basics->parsed && basics->exported_types_count > 0) {
            for (int i = 0; i < basics->exported_types_count; i++) {
                /* Check if we already added it (to avoid duplicates) */
                if (lookup_import(import_map, basics->exported_types[i]) == NULL) {
                    add_import(import_map, basics->exported_types[i], "Basics");
                }
            }
        }
    }

    /* List exposing (List, (::)) - just List type */
    add_import(import_map, "List", "List");
    
    /* Maybe exposing (Maybe(..)) */
    add_import(import_map, "Maybe", "Maybe");
    add_import(import_map, "Just", "Maybe");
    add_import(import_map, "Nothing", "Maybe");
    
    /* Result exposing (Result(..)) */
    add_import(import_map, "Result", "Result");
    add_import(import_map, "Ok", "Result");
    add_import(import_map, "Err", "Result");
    
    /* String exposing (String) */
    add_import(import_map, "String", "String");
    
    /* Char exposing (Char) */
    add_import(import_map, "Char", "Char");
    
    /* Platform exposing (Program) */
    add_import(import_map, "Program", "Platform");
    
    /* Platform.Cmd as Cmd exposing (Cmd) */
    add_import(import_map, "Cmd", "Platform.Cmd");
    
    /* Platform.Sub as Sub exposing (Sub) */
    add_import(import_map, "Sub", "Platform.Sub");
}

/* Helper function to clean documentation comment */
static char *clean_comment(const char *raw_comment) {
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
static char *find_preceding_comment(TSNode node, TSNode root, const char *source_code) {
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

/* Helper function to normalize whitespace - convert newlines and multiple spaces to single spaces */
static char *normalize_whitespace(const char *str) {
    if (!str) return arena_strdup("");

    size_t len = strlen(str);
    char *result = arena_malloc(len + 1);
    size_t pos = 0;
    bool last_was_space = false;

    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!last_was_space && pos > 0) {
                result[pos++] = ' ';
                last_was_space = true;
            }
        } else {
            result[pos++] = c;
            last_was_space = false;
        }
    }

    /* Remove trailing space */
    if (pos > 0 && result[pos - 1] == ' ') {
        pos--;
    }

    result[pos] = '\0';

    /* Remove spaces before commas, ensure spaces around colons in record types, */
    /* and handle spaces before closing parens based on tuple vs function type context */
    char *final = arena_malloc(pos * 3 + 1);  /* Extra space for potential additions */
    size_t final_pos = 0;

    /* First pass: mark which paren pairs contain commas (tuples) */
    /* We match each opening paren with its closing paren */
    bool *is_tuple_paren = arena_malloc(pos * sizeof(bool));
    int *paren_match = arena_malloc(pos * sizeof(int));  /* For each '(', store index of matching ')' */
    bool *has_comma = arena_malloc(pos * sizeof(bool));   /* For each position, whether it contains a comma */

    /* Initialize arrays */
    for (size_t i = 0; i < pos; i++) {
        is_tuple_paren[i] = false;
        paren_match[i] = -1;
        has_comma[i] = false;
    }

    /* Build a stack to match parens and track commas */
    int *paren_stack = arena_malloc(pos * sizeof(int));
    int *brace_depth_stack = arena_malloc(pos * sizeof(int));  /* Track brace depth at each paren level */
    int stack_top = -1;
    int brace_depth = 0;

    for (size_t i = 0; i < pos; i++) {
        if (result[i] == '{') {
            brace_depth++;
        } else if (result[i] == '}') {
            if (brace_depth > 0) brace_depth--;
        } else if (result[i] == '(') {
            stack_top++;
            paren_stack[stack_top] = i;
            brace_depth_stack[stack_top] = brace_depth;
            has_comma[i] = false;  /* This paren pair hasn't seen a comma yet */
        } else if (result[i] == ',') {
            /* Mark that the current paren level has a comma, but only if not inside braces */
            if (stack_top >= 0 && brace_depth == brace_depth_stack[stack_top]) {
                has_comma[paren_stack[stack_top]] = true;
            }
        } else if (result[i] == ')') {
            if (stack_top >= 0) {
                int open_idx = paren_stack[stack_top];
                paren_match[open_idx] = i;
                /* If this paren pair has a comma, mark both as tuple parens */
                if (has_comma[open_idx]) {
                    is_tuple_paren[open_idx] = true;
                    is_tuple_paren[i] = true;
                }
                stack_top--;
            }
        }
    }

    arena_free(paren_stack);
    arena_free(brace_depth_stack);
    arena_free(has_comma);

    /* Track which closing parens to skip (for removing redundant parens in record fields) */
    bool *skip_paren = arena_malloc(pos * sizeof(bool));
    for (size_t i = 0; i < pos; i++) {
        skip_paren[i] = false;
    }

    /* Track brace depth for detecting record fields */
    int current_brace_depth = 0;

    /* Second pass: build final string */
    for (size_t i = 0; i < pos; i++) {
        if (result[i] == '{') {
            current_brace_depth++;
            final[final_pos++] = result[i];
            /* Ensure space after opening brace unless followed by } */
            if (i + 1 < pos && result[i + 1] != '}') {
                /* Skip any existing space */
                if (i + 1 < pos && result[i + 1] == ' ') {
                    i++;
                }
                /* Add exactly one space */
                final[final_pos++] = ' ';
            }
        } else if (result[i] == '}') {
            current_brace_depth--;
            /* Ensure space before closing brace in record types, except for empty records {} */
            if (final_pos > 0 && final[final_pos - 1] != ' ' && final[final_pos - 1] != '{') {
                final[final_pos++] = ' ';
            }
            final[final_pos++] = result[i];
        } else if (result[i] == ' ' && i + 1 < pos && result[i + 1] == '}') {
            /* Keep space before closing brace, unless it's an empty record */
            if (final_pos > 0 && final[final_pos - 1] != '{') {
                final[final_pos++] = result[i];
            }
        } else if (result[i] == ' ' && i + 1 < pos && result[i + 1] == ',') {
            /* Skip space before comma */
            continue;
        } else if (result[i] == ',') {
            /* Add comma and ensure space after it */
            final[final_pos++] = result[i];
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else if (result[i] == ' ' && i + 1 < pos && result[i + 1] == ')') {
            /* Keep space before closing paren only if it's a tuple */
            if (!is_tuple_paren[i + 1]) {
                continue;  /* Skip space before non-tuple closing paren */
            }
            final[final_pos++] = result[i];
        } else if (result[i] == ')') {
            /* Skip if marked for removal (redundant paren in record field) */
            if (skip_paren[i]) {
                /* Skip any space before this closing paren */
                if (final_pos > 0 && final[final_pos - 1] == ' ') {
                    final_pos--;
                }
                continue;
            }
            /* Handle closing paren */
            if (is_tuple_paren[i]) {
                /* Ensure space before closing paren in tuples */
                if (final_pos > 0 && final[final_pos - 1] != ' ') {
                    final[final_pos++] = ' ';
                }
            }
            final[final_pos++] = result[i];
            /* Ensure space after closing paren if followed by -> */
            if (i + 1 < pos && result[i + 1] == '-' && i + 2 < pos && result[i + 2] == '>') {
                /* No space before ->, add one */
                final[final_pos++] = ' ';
            } else if (i + 1 < pos && result[i + 1] == ' ' &&
                       i + 2 < pos && result[i + 2] == '-' &&
                       i + 3 < pos && result[i + 3] == '>') {
                /* Already has space before ->, keep it (will be added in next iteration) */
            }
        } else if (result[i] == '(' && is_tuple_paren[i]) {
            /* Opening paren of a tuple */
            final[final_pos++] = result[i];
            /* Ensure space after opening paren in tuples */
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else if (result[i] == '(' && !is_tuple_paren[i]) {
            /* Opening paren of a non-tuple (function type, parenthesized type) */
            /* Check if we're in a record field (after : and before }) and this paren wraps a function type */
            if (current_brace_depth > 0 && final_pos >= 2 &&
                final[final_pos - 1] == ' ' && final[final_pos - 2] == ':') {
                /* Look ahead to see if this paren wraps a function type */
                /* Find the matching closing paren */
                int paren_depth = 1;
                size_t j = i + 1;
                bool has_arrow = false;
                while (j < pos && paren_depth > 0) {
                    if (result[j] == '(') paren_depth++;
                    else if (result[j] == ')') paren_depth--;
                    else if (paren_depth == 1 && result[j] == '-' && j + 1 < pos && result[j + 1] == '>') {
                        has_arrow = true;
                    }
                    if (paren_depth > 0) j++;
                }
                /* If the paren wraps a function type, check if it's followed by an arrow */
                if (has_arrow && paren_depth == 0) {
                    /* Check if there's a " -> " after the closing paren */
                    /* This indicates the function type is a parameter, not the return type */
                    /* Example: (Int -> String) -> Bool  -- parens are necessary */
                    /*          Int -> (String -> Bool) -- parens are redundant */
                    bool followed_by_arrow = false;
                    if (j + 1 < pos && result[j + 1] == ' ' &&
                        j + 2 < pos && result[j + 2] == '-' &&
                        j + 3 < pos && result[j + 3] == '>') {
                        followed_by_arrow = true;
                    } else if (j + 1 < pos && result[j + 1] == '-' &&
                               j + 2 < pos && result[j + 2] == '>') {
                        followed_by_arrow = true;
                    }

                    /* Only skip parens if NOT followed by an arrow */
                    if (!followed_by_arrow) {
                        /* Mark the closing paren at position j for skipping */
                        skip_paren[j] = true;
                        /* Skip this opening paren and continue */
                        continue;
                    }
                }
            }
            final[final_pos++] = result[i];
            /* Skip any space after opening paren in non-tuples */
            /* This handles the case where type substitution or other processing
             * may have introduced unwanted spaces */
        } else if (result[i] == ' ' && i > 0 && result[i - 1] == '(' && !is_tuple_paren[i - 1]) {
            /* Skip space after non-tuple opening paren */
            continue;
        } else if (result[i] == ':') {
            /* Ensure space before colon if not present */
            if (final_pos > 0 && final[final_pos - 1] != ' ') {
                final[final_pos++] = ' ';
            }
            final[final_pos++] = ':';
            /* Ensure space after colon if not already present */
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else if (result[i] == '-' && i + 1 < pos && result[i + 1] == '>') {
            /* Ensure space before -> if not present */
            if (final_pos > 0 && final[final_pos - 1] != ' ') {
                final[final_pos++] = ' ';
            }
            final[final_pos++] = result[i];
            final[final_pos++] = result[++i];  /* Add the '>' */
            /* Ensure space after -> if not already present */
            if (i + 1 < pos && result[i + 1] != ' ') {
                final[final_pos++] = ' ';
            }
        } else {
            final[final_pos++] = result[i];
        }
    }
    final[final_pos] = '\0';

    arena_free(is_tuple_paren);
    arena_free(paren_match);
    arena_free(skip_paren);
    arena_free(result);

    return final;
}

/* Forward declarations */
static int count_type_arrows(const char *type_str);

/* Helper function to parse a single type argument from a string
 * Returns the end position of the argument, or NULL if no valid argument found
 * Handles: simple identifiers, qualified types, parenthesized types, record types, tuple types */
static const char *parse_type_arg(const char *start, char *out_arg, size_t out_size) {
    const char *p = start;
    
    /* Skip leading whitespace */
    while (*p == ' ') p++;
    
    if (!*p) return NULL;
    
    const char *arg_start = p;
    const char *arg_end = p;
    int depth = 0;
    
    if (*p == '(') {
        /* Parenthesized type or tuple */
        depth = 1;
        arg_end++;
        while (*arg_end && depth > 0) {
            if (*arg_end == '(') depth++;
            else if (*arg_end == ')') depth--;
            arg_end++;
        }
    } else if (*p == '{') {
        /* Record type */
        depth = 1;
        arg_end++;
        while (*arg_end && depth > 0) {
            if (*arg_end == '{') depth++;
            else if (*arg_end == '}') depth--;
            arg_end++;
        }
    } else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        /* Type name, qualified type, or type variable */
        while (*arg_end && ((*arg_end >= 'A' && *arg_end <= 'Z') ||
                           (*arg_end >= 'a' && *arg_end <= 'z') ||
                           (*arg_end >= '0' && *arg_end <= '9') ||
                           *arg_end == '_' || *arg_end == '.')) {
            arg_end++;
        }
        /* Check if this type has type arguments itself (e.g., "List Int") */
        /* Only consume simple type names here; nested type applications are parenthesized */
    } else {
        return NULL;  /* Not a valid type start */
    }
    
    size_t arg_len = arg_end - arg_start;
    if (arg_len == 0 || arg_len >= out_size) return NULL;
    
    memcpy(out_arg, arg_start, arg_len);
    out_arg[arg_len] = '\0';
    
    return arg_end;
}

/* Helper function to substitute type variables with type arguments in an expansion string */
static char *substitute_type_vars(const char *expansion, char **type_vars, int type_vars_count,
                                   char **type_args, int type_args_count) {
    if (type_vars_count == 0 || type_args_count == 0) {
        return arena_strdup(expansion);
    }
    
    /* Use the minimum count to avoid out-of-bounds access */
    int subst_count = type_vars_count < type_args_count ? type_vars_count : type_args_count;
    
    /* Calculate maximum possible size */
    size_t max_arg_len = 0;
    for (int i = 0; i < type_args_count; i++) {
        size_t len = strlen(type_args[i]);
        if (len > max_arg_len) max_arg_len = len;
    }
    
    size_t new_size = strlen(expansion) * 2 + max_arg_len * 20 + 1024;
    char *result = arena_malloc(new_size);
    size_t pos = 0;
    const char *p = expansion;
    
    while (*p && pos < new_size - max_arg_len - 10) {
        /* Check if this is an identifier (potential type variable) */
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            const char *id_start = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                p++;
            }
            size_t id_len = p - id_start;
            
            /* Check if this identifier matches any type variable */
            bool substituted = false;
            for (int i = 0; i < subst_count; i++) {
                size_t var_len = strlen(type_vars[i]);
                if (id_len == var_len && memcmp(id_start, type_vars[i], var_len) == 0) {
                    /* Substitute with the corresponding type argument */
                    size_t arg_len = strlen(type_args[i]);
                    memcpy(result + pos, type_args[i], arg_len);
                    pos += arg_len;
                    substituted = true;
                    break;
                }
            }
            
            if (!substituted) {
                /* Not a type variable - copy as-is */
                memcpy(result + pos, id_start, id_len);
                pos += id_len;
            }
        } else {
            result[pos++] = *p++;
        }
    }
    
    result[pos] = '\0';
    return result;
}

/* Helper function to check if a string contains a function arrow */
static bool contains_function_arrow(const char *str) {
    /* Look for " -> " pattern (with spaces) */
    const char *p = str;
    while (*p && *(p + 1)) {  /* Ensure we have at least 2 chars ahead */
        if (*p == '-' && *(p + 1) == '>' &&
            (p > str && *(p - 1) == ' ') &&
            *(p + 2) && *(p + 2) == ' ') {  /* Check *(p + 2) exists before dereferencing */
            return true;
        }
        p++;
    }
    return false;
}

/* Helper function to expand type aliases that are function types
 * Only expands the final return type, not parameter types
 * Only expands if implementation has more params than type arrows suggest */
static char *expand_function_type_aliases(const char *type_str, TypeAliasMap *type_alias_map, int implementation_param_count) {
    if (!type_str || !type_alias_map) {
        return arena_strdup(type_str ? type_str : "");
    }

    /* Count arrows in the type to determine expected parameter count */
    int arrow_count = count_type_arrows(type_str);

    /* Only expand if implementation has more parameters than the type suggests */
    /* This means the return type alias is being "called" with the extra parameters */
    if (implementation_param_count <= arrow_count) {
        return arena_strdup(type_str);
    }

    /* Find the last occurrence of " -> " (function arrow) */
    const char *last_arrow = NULL;
    const char *p = type_str;
    int paren_depth = 0;

    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            paren_depth--;
        } else if (paren_depth == 0 && *p == '-' && *(p + 1) == '>' &&
                   (p > type_str && *(p - 1) == ' ') && *(p + 2) == ' ') {
            last_arrow = p;
        }
        p++;
    }

    /* If there's no arrow, expand the entire type */
    /* If there's an arrow, only expand the part after the last arrow */
    const char *expand_start = last_arrow ? last_arrow + 3 : type_str;  /* Skip " -> " */
    while (*expand_start == ' ') expand_start++;  /* Skip leading spaces */

    /* Extract the return type */
    size_t prefix_len = expand_start - type_str;

    /* Check if the return type is a type alias */
    const char *return_type_start = expand_start;
    const char *rt = return_type_start;

    /* Skip to the first uppercase letter (start of type name) */
    while (*rt && !((*rt >= 'A' && *rt <= 'Z'))) {
        rt++;
    }

    if (*rt >= 'A' && *rt <= 'Z') {
        /* Extract the type name */
        const char *type_name_start = rt;
        while ((*rt >= 'A' && *rt <= 'Z') || (*rt >= 'a' && *rt <= 'z') ||
               (*rt >= '0' && *rt <= '9') || *rt == '_' || *rt == '.') {
            rt++;
        }

        size_t type_name_len = rt - type_name_start;
        char type_name[256];
        if (type_name_len < sizeof(type_name)) {
            memcpy(type_name, type_name_start, type_name_len);
            type_name[type_name_len] = '\0';

            /* Skip module qualifiers - only look at the last part */
            char *last_dot = strrchr(type_name, '.');
            const char *simple_name = last_dot ? last_dot + 1 : type_name;

            /* Look up the type alias */
            const TypeAliasExpansion *alias = lookup_type_alias(type_alias_map, simple_name);

            if (alias && contains_function_arrow(alias->expansion)) {
                /* This is a function type alias - expand it */

                /* Parse type arguments from the return type if present */
                /* For example: "Decoder Bool" -> type arg is "Bool" */
                /*              "Result String Int" -> type args are "String", "Int" */
                const char *type_args_pos = rt;
                
                /* Collect all type arguments */
                char **type_args = arena_malloc(8 * sizeof(char*));
                int type_args_count = 0;
                int type_args_capacity = 8;
                
                while (*type_args_pos && type_args_count < alias->type_vars_count) {
                    char arg_buf[512];
                    const char *next_pos = parse_type_arg(type_args_pos, arg_buf, sizeof(arg_buf));
                    if (!next_pos) break;
                    
                    if (type_args_count >= type_args_capacity) {
                        type_args_capacity *= 2;
                        type_args = arena_realloc(type_args, type_args_capacity * sizeof(char*));
                    }
                    type_args[type_args_count++] = arena_strdup(arg_buf);
                    type_args_pos = next_pos;
                }

                /* Substitute type variables in the expansion */
                char *expanded = substitute_type_vars(alias->expansion, alias->type_vars, 
                                                       alias->type_vars_count, type_args, type_args_count);

                /* Free type args */
                for (int i = 0; i < type_args_count; i++) {
                    arena_free(type_args[i]);
                }
                arena_free(type_args);

                size_t buf_size = prefix_len + strlen(expanded) + 10;
                char *result = arena_malloc(buf_size);

                /* Copy the prefix (everything before the return type) */
                memcpy(result, type_str, prefix_len);

                /* Copy the expanded type */
                strcpy(result + prefix_len, expanded);

                arena_free(expanded);

                return result;
            }
        }
    }

    /* No expansion needed - return a copy of the original */
    return arena_strdup(type_str);
}

/* Helper function to qualify type names based on import map and local types */
static char *qualify_type_names(const char *type_str, const char *module_name,
                                  ImportMap *import_map, ModuleAliasMap *alias_map,
                                  DirectModuleImports *direct_imports,
                                  char **local_types, int local_types_count,
                                  DependencyCache *dep_cache) {
    size_t buf_size = strlen(type_str) * 3 + 1024;  /* Extra space for qualifications */
    char *result = arena_malloc(buf_size);
    size_t pos = 0;
    const char *p = type_str;

    while (*p && pos < buf_size - 200) {
        if (*p >= 'A' && *p <= 'Z') {
            /* Check if this uppercase letter is part of a camelCase identifier */
            /* by checking if the previous character was alphanumeric */
            bool is_camel_case = false;
            if (p > type_str) {
                char prev = *(p - 1);
                if ((prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9') || prev == '_') {
                    is_camel_case = true;
                }
            }

            if (is_camel_case) {
                /* Part of camelCase identifier - copy as-is */
                result[pos++] = *p++;
            } else {
                /* Found an uppercase identifier - might be a type */
                const char *start = p;
                while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                       (*p >= '0' && *p <= '9') || *p == '_') {
                    p++;
                }
                size_t len = p - start;
                char typename[256];
                if (len < sizeof(typename)) {
                    memcpy(typename, start, len);
                    typename[len] = '\0';

                    /* Check if this is part of an already-qualified type in the SOURCE */
                    /* Look backward: if there's a dot before this, it's already qualified */
                    bool already_qualified = false;
                    if (start > type_str && *(start - 1) == '.') {
                        already_qualified = true;
                    }

                    /* Check if this is a module prefix (followed by a dot) */
                    bool is_module_prefix = (*p == '.');

                    if (already_qualified) {
                        /* Keep as-is - already qualified */
                        memcpy(result + pos, start, len);
                        pos += len;
                    } else if (is_module_prefix) {
                        /* This is a module prefix - check if it's an alias that should be expanded.
                         * Since aliased modules are NOT added to direct_imports (the original name
                         * is unavailable in Elm when aliased), we expand whenever we find an alias.
                         * The is_directly_imported check handles the rare case where a module is
                         * imported both directly AND via an alias (two separate import statements). */

                        /* Extract the type name after the dot for ambiguous alias resolution */
                        const char *referenced_type = NULL;
                        char type_after_dot[256];
                        if (*p == '.') {
                            const char *after_dot = p + 1;
                            /* Skip to the next uppercase letter (handle cases like "C . Dot") */
                            while (*after_dot && (*after_dot == ' ' || *after_dot == '\t')) {
                                after_dot++;
                            }
                            if (*after_dot >= 'A' && *after_dot <= 'Z') {
                                const char *type_start = after_dot;
                                while ((*after_dot >= 'A' && *after_dot <= 'Z') ||
                                       (*after_dot >= 'a' && *after_dot <= 'z') ||
                                       (*after_dot >= '0' && *after_dot <= '9') ||
                                       *after_dot == '_') {
                                    after_dot++;
                                }
                                size_t type_len = after_dot - type_start;
                                if (type_len > 0 && type_len < sizeof(type_after_dot)) {
                                    memcpy(type_after_dot, type_start, type_len);
                                    type_after_dot[type_len] = '\0';
                                    referenced_type = type_after_dot;
                                }
                            }
                        }

                        bool is_ambiguous = false;
                        const char *ambig_mod1 = NULL;
                        const char *ambig_mod2 = NULL;
                        const char *full_module = lookup_module_alias(alias_map, typename, referenced_type,
                                                                      dep_cache, &is_ambiguous,
                                                                      &ambig_mod1, &ambig_mod2);

                        if (is_ambiguous) {
                            /* AMBIGUOUS: Two different modules use the same alias.
                             * This matches Elm compiler behavior - it's an error to use this alias.
                             * We report it to stderr and keep the alias unexpanded. */
                            fprintf(stderr, "Warning: Ambiguous alias '%s' - refers to both '%s' and '%s'\n",
                                    typename, ambig_mod1 ? ambig_mod1 : "?", ambig_mod2 ? ambig_mod2 : "?");
                            /* Keep as-is - cannot resolve */
                            memcpy(result + pos, start, len);
                            pos += len;
                        } else {
                            bool should_expand = (full_module != NULL) &&
                                                !is_directly_imported(direct_imports, typename);

                            if (should_expand) {
                                /* Expand the alias to the full module name */
                                size_t flen = strlen(full_module);
                                memcpy(result + pos, full_module, flen);
                                pos += flen;
                            } else {
                                /* Keep as-is - either not an alias, or also directly imported */
                                memcpy(result + pos, start, len);
                                pos += len;
                            }
                        }
                    } else {
                        /* Check if it's a local type first - local types take precedence over imports */
                        bool is_local = false;
                        for (int i = 0; i < local_types_count; i++) {
                            if (strcmp(typename, local_types[i]) == 0) {
                                is_local = true;
                                break;
                            }
                        }

                        if (is_local) {
                            /* Qualify with current module */
                            pos += snprintf(result + pos, buf_size - pos, "%s.%s", module_name, typename);
                        } else {
                            /* Check if it's imported (including implicit imports) */
                            const char *import_module = lookup_import(import_map, typename);
                            if (import_module) {
                                /* Use the imported module name */
                                pos += snprintf(result + pos, buf_size - pos, "%s.%s", import_module, typename);
                            } else {
                                /* Unknown type - keep as-is (likely a type variable) */
                                memcpy(result + pos, start, len);
                                pos += len;
                            }
                        }
                    }
                }
            }
        } else {
            result[pos++] = *p++;
        }
    }
    result[pos] = '\0';
    return result;
}

/* Helper function to collect comment byte ranges */
typedef struct {
    uint32_t start;
    uint32_t end;
} ByteRange;

static void collect_comment_ranges(TSNode node, ByteRange *ranges, int *count, int capacity) {
    const char *node_type = ts_node_type(node);

    /* If this is a comment node, record its range */
    if (strcmp(node_type, "block_comment") == 0 || strcmp(node_type, "line_comment") == 0) {
        if (*count < capacity) {
            ranges[*count].start = ts_node_start_byte(node);
            ranges[*count].end = ts_node_end_byte(node);
            (*count)++;
        }
        return;  /* Don't recurse into comment nodes */
    }

    /* Recursively check children */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        collect_comment_ranges(child, ranges, count, capacity);
    }
}

/* Helper function to extract text from node, skipping comment ranges */
static char *extract_text_skip_comments(TSNode node, const char *source_code) {
    uint32_t node_start = ts_node_start_byte(node);
    uint32_t node_end = ts_node_end_byte(node);
    uint32_t node_length = node_end - node_start;

    /* Collect all comment ranges within this node */
    ByteRange comment_ranges[64];
    int comment_count = 0;
    collect_comment_ranges(node, comment_ranges, &comment_count, 64);

    /* Allocate buffer for result */
    char *buffer = arena_malloc(node_length + 1);
    size_t pos = 0;

    /* Copy text, skipping comment ranges */
    uint32_t current = node_start;
    for (int i = 0; i < comment_count; i++) {
        /* Copy text before this comment */
        if (current < comment_ranges[i].start) {
            uint32_t len = comment_ranges[i].start - current;
            memcpy(buffer + pos, source_code + current, len);
            pos += len;
        }
        /* Skip the comment itself and move to after it */
        current = comment_ranges[i].end;
    }

    /* Copy remaining text after last comment */
    if (current < node_end) {
        uint32_t len = node_end - current;
        memcpy(buffer + pos, source_code + current, len);
        pos += len;
    }

    buffer[pos] = '\0';
    return buffer;
}

/* Helper function to count function arrows in type string (excluding arrows inside parens) */
static int count_type_arrows(const char *type_str) {
    int arrow_count = 0;
    int paren_depth = 0;
    const char *p = type_str;

    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            paren_depth--;
        } else if (paren_depth == 0 && *p == '-' && *(p + 1) == '>' &&
                   (p > type_str && *(p - 1) == ' ') && *(p + 2) == ' ') {
            arrow_count++;
        }
        p++;
    }
    return arrow_count;
}

/* Helper function to count implementation parameters */
static int count_implementation_params(TSNode value_decl_node, const char *source_code) {
    (void)source_code;
    int param_count = 0;

    /* Find function_declaration_left */
    uint32_t child_count = ts_node_child_count(value_decl_node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(value_decl_node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "function_declaration_left") == 0) {
            /* Count children that are parameters (lower_pattern, pattern, etc.) */
            /* Skip the first child which is the function name */
            uint32_t func_child_count = ts_node_child_count(child);
            bool found_func_name = false;
            for (uint32_t j = 0; j < func_child_count; j++) {
                TSNode func_child = ts_node_child(child, j);
                const char *func_child_type = ts_node_type(func_child);

                /* Skip the function name (first lower_case_identifier) */
                if (!found_func_name && strcmp(func_child_type, "lower_case_identifier") == 0) {
                    found_func_name = true;
                    continue;
                }

                /* Count anything that looks like a parameter */
                if (strcmp(func_child_type, "lower_pattern") == 0 ||
                    strcmp(func_child_type, "pattern") == 0 ||
                    strcmp(func_child_type, "lower_case_identifier") == 0 ||
                    strcmp(func_child_type, "anything_pattern") == 0 ||
                    strcmp(func_child_type, "tuple_pattern") == 0 ||
                    strcmp(func_child_type, "list_pattern") == 0 ||
                    strcmp(func_child_type, "record_pattern") == 0 ||
                    strcmp(func_child_type, "union_pattern") == 0) {
                    param_count++;
                }
            }
            break;
        }
    }

    return param_count;
}

/* Helper function to remove unnecessary outer parentheses from return type
 * Example: "A -> B -> (C -> D)" becomes "A -> B -> C -> D"
 * This matches Elm's canonical documentation format */
static char *remove_return_type_parens(const char *type_str) {
    if (!type_str) return arena_strdup("");

    /* First, check if the entire type is wrapped in outer parentheses */
    /* This can happen when expanding type aliases that are function types */
    if (*type_str == '(') {
        const char *scan = type_str + 1;
        int depth = 1;
        bool has_comma = false;  /* Track if there's a comma inside (indicates tuple) */

        while (*scan && depth > 0) {
            if (*scan == '(') depth++;
            else if (*scan == ')') {
                depth--;
                if (depth == 0 && *(scan + 1) == '\0') {
                    /* The parens wrap the entire type */
                    /* Only unwrap if it's not a tuple (no comma at top level) */
                    if (!has_comma) {
                        /* Extract the inner type and recursively process it */
                        size_t inner_len = scan - type_str - 1;
                        /* Don't unwrap empty parens - that's the unit type () */
                        if (inner_len == 0) {
                            return arena_strdup("()");
                        }
                        char *inner = arena_malloc(inner_len + 1);
                        memcpy(inner, type_str + 1, inner_len);
                        inner[inner_len] = '\0';
                        char *result = remove_return_type_parens(inner);
                        arena_free(inner);
                        return result;
                    }
                    break;
                }
            } else if (*scan == ',' && depth == 1) {
                /* Comma at the top level inside these parens - this is a tuple */
                has_comma = true;
            }
            scan++;
        }
    }

    /* Find the last top-level arrow */
    const char *last_arrow = NULL;
    const char *p = type_str;
    int paren_depth = 0;
    int brace_depth = 0;

    while (*p) {
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            paren_depth--;
        } else if (*p == '{') {
            brace_depth++;
        } else if (*p == '}') {
            brace_depth--;
        } else if (paren_depth == 0 && brace_depth == 0 &&
                   *p == '-' && *(p + 1) == '>' &&
                   (p > type_str && *(p - 1) == ' ') && *(p + 2) == ' ') {
            last_arrow = p;
        }
        p++;
    }

    /* If there's no arrow, return as-is */
    if (!last_arrow) {
        return arena_strdup(type_str);
    }

    /* Find the start of the return type (skip " -> " and leading spaces) */
    const char *return_start = last_arrow + 3;  /* Skip " -> " */
    while (*return_start == ' ') return_start++;

    /* Check if the return type is wrapped in unnecessary outer parentheses */
    if (*return_start != '(') {
        return arena_strdup(type_str);
    }

    /* Check if these parens wrap the entire return type */
    const char *scan = return_start + 1;
    int depth = 1;
    const char *return_end = NULL;
    bool has_comma = false;  /* Track if there's a comma inside (indicates tuple) */

    while (*scan && depth > 0) {
        if (*scan == '(') depth++;
        else if (*scan == ')') {
            depth--;
            if (depth == 0) {
                return_end = scan;
            }
        } else if (*scan == ',' && depth == 1) {
            /* Comma at the top level inside these parens - this is a tuple */
            has_comma = true;
        }
        scan++;
    }

    /* Only remove parens if:
     * 1. They wrap the entire return type
     * 2. They don't contain a comma (not a tuple)
     * 3. The inner content is not empty (preserve unit type ()) */
    if (return_end && *(return_end + 1) == '\0' && !has_comma) {
        /* Build the new type string without these outer parens */
        size_t prefix_len = return_start - type_str;
        size_t inner_len = return_end - return_start - 1;  /* Skip opening '(' */

        /* Don't unwrap empty parens in return type - that's the unit type () */
        if (inner_len == 0) {
            return arena_strdup(type_str);
        }

        char *result = arena_malloc(prefix_len + inner_len + 1);
        memcpy(result, type_str, prefix_len);
        memcpy(result + prefix_len, return_start + 1, inner_len);
        result[prefix_len + inner_len] = '\0';

        return result;
    }

    return arena_strdup(type_str);
}

/* Helper function to extract and canonicalize type expression */
static char *extract_type_expression(TSNode type_node, const char *source_code, const char *module_name,
                                       ImportMap *import_map, ModuleAliasMap *alias_map,
                                       DirectModuleImports *direct_imports,
                                       char **local_types, int local_types_count,
                                       TypeAliasMap *type_alias_map, int implementation_param_count,
                                       DependencyCache *dep_cache) {
    if (ts_node_is_null(type_node)) {
        return arena_strdup("");
    }

    /* Extract the raw type text, skipping comments */
    char *raw_text = extract_text_skip_comments(type_node, source_code);

    /* Normalize whitespace */
    char *normalized = normalize_whitespace(raw_text);
    arena_free(raw_text);

    /* Expand function type aliases (only if we have implementation param count) */
    char *expanded = expand_function_type_aliases(normalized, type_alias_map, implementation_param_count);
    arena_free(normalized);

    /* Qualify type names */
    char *qualified = qualify_type_names(expanded, module_name, import_map, alias_map, direct_imports, local_types, local_types_count, dep_cache);
    arena_free(expanded);

    /* Remove unnecessary outer parentheses from return type */
    char *canonical = remove_return_type_parens(qualified);
    arena_free(qualified);

    return canonical;
}

/* Extract value declaration (function/constant) */
static bool extract_value_decl(TSNode node, const char *source_code, ElmValue *value, const char *module_name,
                                 ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                                 char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                                 DependencyCache *dep_cache) {
    /* Find type_annotation sibling first */
    TSNode type_annotation = ts_node_prev_named_sibling(node);
    if (ts_node_is_null(type_annotation) ||
        strcmp(ts_node_type(type_annotation), "type_annotation") != 0) {
        /* No type annotation found - skip this value */
        return false;
    }

    /* Extract function name from value_declaration */
    char *func_name = NULL;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "function_declaration_left") == 0) {
            /* Find the function name */
            uint32_t func_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < func_child_count; j++) {
                TSNode func_child = ts_node_child(child, j);
                if (strcmp(ts_node_type(func_child), "lower_case_identifier") == 0) {
                    func_name = get_node_text(func_child, source_code);
                    break;
                }
            }
            break;
        }
    }

    if (!func_name) {
        return false;
    }

    /* Count implementation parameters */
    int impl_param_count = count_implementation_params(node, source_code);

    /* Extract type from type_annotation */
    char *type_str = NULL;
    uint32_t ann_child_count = ts_node_child_count(type_annotation);
    for (uint32_t i = 0; i < ann_child_count; i++) {
        TSNode child = ts_node_child(type_annotation, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "type_expression") == 0) {
            type_str = extract_type_expression(child, source_code, module_name, import_map, alias_map, direct_imports, local_types, local_types_count, type_alias_map, impl_param_count, dep_cache);
            break;
        }
    }

    if (!type_str) {
        arena_free(func_name);
        return false;
    }

    /* Extract comment */
    char *comment = find_preceding_comment(type_annotation, ts_node_parent(node), source_code);

    value->name = func_name;
    value->type = type_str;
    value->comment = comment;

    return true;
}

/* Extract type alias */
static bool extract_type_alias(TSNode node, const char *source_code, ElmAlias *alias, const char *module_name,
                                 ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                                 char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                                 DependencyCache *dep_cache) {
    (void)type_alias_map;  /* Not used - avoid circular expansion when extracting alias definitions */
    char *alias_name = NULL;
    char *type_expr = NULL;
    char **args = NULL;
    int args_count = 0;
    int args_capacity = 0;

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "upper_case_identifier") == 0 && !alias_name) {
            alias_name = get_node_text(child, source_code);
        } else if (strcmp(child_type, "lower_type_name") == 0) {
            /* Type parameter */
            if (args_count >= args_capacity) {
                args_capacity = args_capacity == 0 ? 8 : args_capacity * 2;
                args = arena_realloc(args, args_capacity * sizeof(char*));
            }
            args[args_count++] = get_node_text(child, source_code);
        } else if (strcmp(child_type, "type_expression") == 0) {
            /* Don't expand aliases when extracting alias definitions to avoid circular expansion */
            type_expr = extract_type_expression(child, source_code, module_name, import_map, alias_map, direct_imports, local_types, local_types_count, NULL, 0, dep_cache);
        }
    }

    if (!alias_name || !type_expr) {
        arena_free(alias_name);
        arena_free(type_expr);
        for (int i = 0; i < args_count; i++) arena_free(args[i]);
        arena_free(args);
        return false;
    }

    char *comment = find_preceding_comment(node, ts_node_parent(node), source_code);

    alias->name = alias_name;
    alias->args = args;
    alias->args_count = args_count;
    alias->type = type_expr;
    alias->comment = comment;

    return true;
}

/* Extract union type */
static bool extract_union_type(TSNode node, const char *source_code, ElmUnion *union_type, const char *module_name,
                                 ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                                 char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                                 DependencyCache *dep_cache) {
    char *type_name = NULL;
    char **args = NULL;
    int args_count = 0;
    int args_capacity = 0;

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *child_type = ts_node_type(child);

        if (strcmp(child_type, "upper_case_identifier") == 0 && !type_name) {
            type_name = get_node_text(child, source_code);
        } else if (strcmp(child_type, "lower_type_name") == 0) {
            /* Type parameter */
            if (args_count >= args_capacity) {
                args_capacity = args_capacity == 0 ? 8 : args_capacity * 2;
                args = arena_realloc(args, args_capacity * sizeof(char*));
            }
            args[args_count++] = get_node_text(child, source_code);
        } else if (strcmp(child_type, "union_variant") == 0) {
            /* We have constructors - extract them */
            /* For now, we'll handle this later when processing the union_variant list */
        }
    }

    if (!type_name) {
        for (int i = 0; i < args_count; i++) arena_free(args[i]);
        arena_free(args);
        return false;
    }

    char *comment = find_preceding_comment(node, ts_node_parent(node), source_code);

    union_type->name = type_name;
    union_type->args = args;
    union_type->args_count = args_count;
    union_type->comment = comment;
    union_type->cases = NULL;
    union_type->cases_count = 0;

    /* Extract union constructors */
    int cases_capacity = 4;
    ElmUnionCase *cases = arena_malloc(cases_capacity * sizeof(ElmUnionCase));
    int cases_count = 0;

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), "union_variant") == 0) {
            if (cases_count >= cases_capacity) {
                cases_capacity *= 2;
                cases = arena_realloc(cases, cases_capacity * sizeof(ElmUnionCase));
            }

            /* Extract constructor name and arguments */
            char *constructor_name = NULL;
            char **arg_types = NULL;
            int arg_types_count = 0;

            uint32_t variant_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < variant_child_count; j++) {
                TSNode variant_child = ts_node_child(child, j);
                const char *variant_child_type = ts_node_type(variant_child);

                if (strcmp(variant_child_type, "upper_case_identifier") == 0 && !constructor_name) {
                    constructor_name = get_node_text(variant_child, source_code);
                } else if (strcmp(variant_child_type, "type_expression") == 0 ||
                           strcmp(variant_child_type, "type_ref") == 0 ||
                           strcmp(variant_child_type, "record_type") == 0 ||
                           strcmp(variant_child_type, "tuple_type") == 0 ||
                           strcmp(variant_child_type, "type_variable") == 0) {
                    /* Constructor argument (type expression, ref, record, tuple, or type variable) */
                    if (arg_types_count == 0) {
                        arg_types = arena_malloc(8 * sizeof(char*));
                    }
                    arg_types[arg_types_count++] = extract_type_expression(variant_child, source_code, module_name,
                                                                             import_map, alias_map, direct_imports, local_types, local_types_count, type_alias_map, 0, dep_cache);
                }
            }

            if (constructor_name) {
                cases[cases_count].name = constructor_name;
                cases[cases_count].arg_types = arg_types;
                cases[cases_count].arg_types_count = arg_types_count;
                cases_count++;
            }
        }
    }

    union_type->cases = cases;
    union_type->cases_count = cases_count;

    return true;
}

/* Main parsing function */
bool parse_elm_file(const char *filepath, ElmModuleDocs *docs, DependencyCache *dep_cache) {
    /* Initialize the docs structure */
    memset(docs, 0, sizeof(ElmModuleDocs));

    /* Read file content */
    char *source_code = read_file(filepath);
    if (!source_code) {
        return false;
    }

    /* Create parser */
    TSParser *parser = ts_parser_new();
    if (!parser) {
        arena_free(source_code);
        return false;
    }

    /* Set the Elm language */
    if (!ts_parser_set_language(parser, tree_sitter_elm())) {
        fprintf(stderr, "Error: Failed to set Elm language\n");
        ts_parser_delete(parser);
        arena_free(source_code);
        return false;
    }

    /* Parse the source code */
    TSTree *tree = ts_parser_parse_string(
        parser,
        NULL,
        source_code,
        strlen(source_code)
    );

    if (!tree) {
        fprintf(stderr, "Error: Failed to parse file %s\n", filepath);
        ts_parser_delete(parser);
        arena_free(source_code);
        return false;
    }

    /* Get the root node */
    TSNode root_node = ts_tree_root_node(tree);

    /* Extract module name and export list */
    ExportList exports;
    docs->name = extract_module_info(root_node, source_code, &exports);

    /* Parse imports, module aliases, and direct imports */
    ImportMap import_map;
    init_import_map(&import_map);
    ModuleAliasMap alias_map;
    init_module_alias_map(&alias_map);
    DirectModuleImports direct_imports;
    init_direct_imports(&direct_imports);
    
    /* Apply Elm's implicit imports first (lowest priority) */
    apply_implicit_imports(&import_map, &alias_map, &direct_imports, dep_cache);
    
    /* Then parse explicit imports (will override implicit ones if there's a conflict) */
    extract_imports(root_node, source_code, &import_map, &alias_map, &direct_imports, dep_cache);

    /* Extract module-level comment (comes AFTER module declaration) */
    uint32_t child_count = ts_node_child_count(root_node);
    docs->comment = NULL;
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        if (strcmp(ts_node_type(child), "module_declaration") == 0) {
            /* Look for block_comment after the module declaration */
            /* Skip over any line comments or other nodes to find the first block_comment */
            for (uint32_t j = i + 1; j < child_count; j++) {
                TSNode next = ts_node_child(root_node, j);
                const char *next_type = ts_node_type(next);
                if (strcmp(next_type, "block_comment") == 0) {
                    char *raw = get_node_text(next, source_code);
                    docs->comment = clean_comment(raw);
                    arena_free(raw);
                    break;
                }
                /* Stop searching if we hit a declaration (not a comment or import) */
                if (strcmp(next_type, "value_declaration") == 0 ||
                    strcmp(next_type, "type_alias_declaration") == 0 ||
                    strcmp(next_type, "type_declaration") == 0) {
                    break;
                }
            }
            break;
        }
    }
    if (!docs->comment) {
        docs->comment = arena_strdup("");
    }

    /* Allocate dynamic arrays for declarations */
    int values_capacity = 16;
    int aliases_capacity = 8;
    int unions_capacity = 8;

    docs->values = arena_malloc(values_capacity * sizeof(ElmValue));
    docs->aliases = arena_malloc(aliases_capacity * sizeof(ElmAlias));
    docs->unions = arena_malloc(unions_capacity * sizeof(ElmUnion));
    docs->values_count = 0;
    docs->aliases_count = 0;
    docs->unions_count = 0;

    /* First pass: collect local type names and build type alias map */
    int local_types_capacity = 16;
    int local_types_count = 0;
    char **local_types = arena_malloc(local_types_capacity * sizeof(char*));

    TypeAliasMap type_alias_map;
    init_type_alias_map(&type_alias_map);

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "type_alias_declaration") == 0 || strcmp(type, "type_declaration") == 0) {
            /* Extract type name */
            uint32_t type_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < type_child_count; j++) {
                TSNode type_child = ts_node_child(child, j);
                if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                    char *type_name = get_node_text(type_child, source_code);
                    if (local_types_count >= local_types_capacity) {
                        local_types_capacity *= 2;
                        local_types = arena_realloc(local_types, local_types_capacity * sizeof(char*));
                    }
                    local_types[local_types_count++] = type_name;
                    break;
                }
            }
        }

        /* Build type alias map for function type aliases */
        if (strcmp(type, "type_alias_declaration") == 0) {
            char *alias_name = NULL;
            char **type_vars = arena_malloc(8 * sizeof(char*));
            int type_vars_count = 0;
            char *expansion = NULL;

            uint32_t type_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < type_child_count; j++) {
                TSNode type_child = ts_node_child(child, j);
                const char *child_type = ts_node_type(type_child);

                if (strcmp(child_type, "upper_case_identifier") == 0 && !alias_name) {
                    alias_name = get_node_text(type_child, source_code);
                } else if (strcmp(child_type, "lower_type_name") == 0) {
                    type_vars[type_vars_count++] = get_node_text(type_child, source_code);
                } else if (strcmp(child_type, "type_expression") == 0) {
                    /* Extract raw expansion without qualification */
                    char *raw_text = extract_text_skip_comments(type_child, source_code);
                    expansion = normalize_whitespace(raw_text);
                    arena_free(raw_text);
                }
            }

            if (alias_name && expansion) {
                add_type_alias(&type_alias_map, alias_name, type_vars, type_vars_count, expansion);
                arena_free(alias_name);
                arena_free(expansion);
            } else {
                if (alias_name) arena_free(alias_name);
                if (expansion) arena_free(expansion);
                for (int k = 0; k < type_vars_count; k++) {
                    arena_free(type_vars[k]);
                }
                arena_free(type_vars);
            }
        }
    }

    /* Second pass: Walk the tree to extract declarations */
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "value_declaration") == 0) {
            /* Found a function/value declaration */
            ElmValue value;
            if (extract_value_decl(child, source_code, &value, docs->name, &import_map, &alias_map, &direct_imports, local_types, local_types_count, &type_alias_map, dep_cache)) {
                /* Only include if exported */
                if (is_exported_value(value.name, &exports)) {
                    if (docs->values_count >= values_capacity) {
                        values_capacity *= 2;
                        docs->values = arena_realloc(docs->values, values_capacity * sizeof(ElmValue));
                    }
                    docs->values[docs->values_count++] = value;
                } else {
                    /* Not exported, free the value */
                    arena_free(value.name);
                    arena_free(value.type);
                    arena_free(value.comment);
                }
            }
        } else if (strcmp(type, "type_alias_declaration") == 0) {
            /* Found a type alias */
            ElmAlias alias;
            if (extract_type_alias(child, source_code, &alias, docs->name, &import_map, &alias_map, &direct_imports, local_types, local_types_count, &type_alias_map, dep_cache)) {
                /* Only include if exported */
                if (is_exported_type(alias.name, &exports)) {
                    if (docs->aliases_count >= aliases_capacity) {
                        aliases_capacity *= 2;
                        docs->aliases = arena_realloc(docs->aliases, aliases_capacity * sizeof(ElmAlias));
                    }
                    docs->aliases[docs->aliases_count++] = alias;
                } else {
                    /* Not exported, free the alias */
                    arena_free(alias.name);
                    arena_free(alias.comment);
                    for (int j = 0; j < alias.args_count; j++) {
                        arena_free(alias.args[j]);
                    }
                    arena_free(alias.args);
                    arena_free(alias.type);
                }
            }
        } else if (strcmp(type, "type_declaration") == 0) {
            /* Found a union type */
            ElmUnion union_type;
            if (extract_union_type(child, source_code, &union_type, docs->name, &import_map, &alias_map, &direct_imports, local_types, local_types_count, &type_alias_map, dep_cache)) {
                /* Only include if exported */
                if (is_exported_type(union_type.name, &exports)) {
                    /* Check if constructors should be exposed */
                    if (!is_type_exposed_with_constructors(union_type.name, &exports)) {
                        /* Opaque type - clear constructors */
                        for (int j = 0; j < union_type.cases_count; j++) {
                            arena_free(union_type.cases[j].name);
                            for (int k = 0; k < union_type.cases[j].arg_types_count; k++) {
                                arena_free(union_type.cases[j].arg_types[k]);
                            }
                            arena_free(union_type.cases[j].arg_types);
                        }
                        arena_free(union_type.cases);
                        union_type.cases = NULL;
                        union_type.cases_count = 0;
                    }

                    if (docs->unions_count >= unions_capacity) {
                        unions_capacity *= 2;
                        docs->unions = arena_realloc(docs->unions, unions_capacity * sizeof(ElmUnion));
                    }
                    docs->unions[docs->unions_count++] = union_type;
                } else {
                    /* Not exported, free the union type */
                    arena_free(union_type.name);
                    arena_free(union_type.comment);
                    for (int j = 0; j < union_type.args_count; j++) {
                        arena_free(union_type.args[j]);
                    }
                    arena_free(union_type.args);
                    for (int j = 0; j < union_type.cases_count; j++) {
                        arena_free(union_type.cases[j].name);
                        for (int k = 0; k < union_type.cases[j].arg_types_count; k++) {
                            arena_free(union_type.cases[j].arg_types[k]);
                        }
                        arena_free(union_type.cases[j].arg_types);
                    }
                    arena_free(union_type.cases);
                }
            }
        }
    }

    /* Clean up local types list */
    for (int i = 0; i < local_types_count; i++) {
        arena_free(local_types[i]);
    }
    arena_free(local_types);

    /* Clean up import map, alias map, direct imports, and type alias map */
    free_import_map(&import_map);
    free_module_alias_map(&alias_map);
    free_direct_imports(&direct_imports);
    free_type_alias_map(&type_alias_map);

    /* Sort declarations alphabetically by name */
    /* Sort values */
    for (int i = 0; i < docs->values_count - 1; i++) {
        for (int j = i + 1; j < docs->values_count; j++) {
            if (strcmp(docs->values[i].name, docs->values[j].name) > 0) {
                ElmValue temp = docs->values[i];
                docs->values[i] = docs->values[j];
                docs->values[j] = temp;
            }
        }
    }

    /* Sort aliases */
    for (int i = 0; i < docs->aliases_count - 1; i++) {
        for (int j = i + 1; j < docs->aliases_count; j++) {
            if (strcmp(docs->aliases[i].name, docs->aliases[j].name) > 0) {
                ElmAlias temp = docs->aliases[i];
                docs->aliases[i] = docs->aliases[j];
                docs->aliases[j] = temp;
            }
        }
    }

    /* Sort unions */
    for (int i = 0; i < docs->unions_count - 1; i++) {
        for (int j = i + 1; j < docs->unions_count; j++) {
            if (strcmp(docs->unions[i].name, docs->unions[j].name) > 0) {
                ElmUnion temp = docs->unions[i];
                docs->unions[i] = docs->unions[j];
                docs->unions[j] = temp;
            }
        }
    }

    /* Sort binops */
    for (int i = 0; i < docs->binops_count - 1; i++) {
        for (int j = i + 1; j < docs->binops_count; j++) {
            if (strcmp(docs->binops[i].name, docs->binops[j].name) > 0) {
                ElmBinop temp = docs->binops[i];
                docs->binops[i] = docs->binops[j];
                docs->binops[j] = temp;
            }
        }
    }

    fprintf(stderr, "Successfully parsed: %s (Module: %s, %d values, %d aliases, %d unions)\n",
            filepath, docs->name, docs->values_count, docs->aliases_count, docs->unions_count);

    /* Clean up export list */
    free_export_list(&exports);

    /* Cleanup */
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    arena_free(source_code);

    return true;
}

/* Free documentation structure */
void free_elm_docs(ElmModuleDocs *docs) {
    if (!docs) return;

    arena_free(docs->name);
    arena_free(docs->comment);

    for (int i = 0; i < docs->values_count; i++) {
        arena_free(docs->values[i].name);
        arena_free(docs->values[i].comment);
        arena_free(docs->values[i].type);
    }
    arena_free(docs->values);

    for (int i = 0; i < docs->aliases_count; i++) {
        arena_free(docs->aliases[i].name);
        arena_free(docs->aliases[i].comment);
        for (int j = 0; j < docs->aliases[i].args_count; j++) {
            arena_free(docs->aliases[i].args[j]);
        }
        arena_free(docs->aliases[i].args);
        arena_free(docs->aliases[i].type);
    }
    arena_free(docs->aliases);

    for (int i = 0; i < docs->unions_count; i++) {
        arena_free(docs->unions[i].name);
        arena_free(docs->unions[i].comment);
        for (int j = 0; j < docs->unions[i].args_count; j++) {
            arena_free(docs->unions[i].args[j]);
        }
        arena_free(docs->unions[i].args);
        for (int j = 0; j < docs->unions[i].cases_count; j++) {
            arena_free(docs->unions[i].cases[j].name);
            for (int k = 0; k < docs->unions[i].cases[j].arg_types_count; k++) {
                arena_free(docs->unions[i].cases[j].arg_types[k]);
            }
            arena_free(docs->unions[i].cases[j].arg_types);
        }
        arena_free(docs->unions[i].cases);
    }
    arena_free(docs->unions);

    for (int i = 0; i < docs->binops_count; i++) {
        arena_free(docs->binops[i].name);
        arena_free(docs->binops[i].comment);
        arena_free(docs->binops[i].type);
        arena_free(docs->binops[i].associativity);
    }
    arena_free(docs->binops);
}

/* Helper function to escape JSON string */
static void print_json_string(const char *str) {
    if (!str) {
        printf("\"\"");
        return;
    }

    printf("\"");
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"':  printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\b': printf("\\b"); break;
            case '\f': printf("\\f"); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if ((unsigned char)*p < 32) {
                    printf("\\u%04x", *p);
                } else {
                    putchar(*p);
                }
        }
    }
    printf("\"");
}

/* Print docs to JSON stdout */
void print_docs_json(ElmModuleDocs *docs, int docs_count) {
    printf("[\n");

    for (int i = 0; i < docs_count; i++) {
        printf("  {\n");

        /* Module name */
        printf("    \"name\": ");
        print_json_string(docs[i].name);
        printf(",\n");

        /* Module comment */
        printf("    \"comment\": ");
        print_json_string(docs[i].comment);
        printf(",\n");

        /* Unions */
        printf("    \"unions\": [");
        for (int j = 0; j < docs[i].unions_count; j++) {
            ElmUnion *u = &docs[i].unions[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(u->name);
            printf(",\n        \"comment\": ");
            print_json_string(u->comment);
            printf(",\n        \"args\": [");
            for (int k = 0; k < u->args_count; k++) {
                if (k > 0) printf(", ");
                print_json_string(u->args[k]);
            }
            printf("],\n        \"cases\": [");
            for (int k = 0; k < u->cases_count; k++) {
                if (k > 0) printf(", ");
                printf("[");
                print_json_string(u->cases[k].name);
                printf(", [");
                for (int m = 0; m < u->cases[k].arg_types_count; m++) {
                    if (m > 0) printf(", ");
                    print_json_string(u->cases[k].arg_types[m]);
                }
                printf("]]");
            }
            printf("]\n      }");
        }
        if (docs[i].unions_count > 0) {
            printf("\n    ");
        }
        printf("],\n");

        /* Aliases */
        printf("    \"aliases\": [");
        for (int j = 0; j < docs[i].aliases_count; j++) {
            ElmAlias *a = &docs[i].aliases[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(a->name);
            printf(",\n        \"comment\": ");
            print_json_string(a->comment);
            printf(",\n        \"args\": [");
            for (int k = 0; k < a->args_count; k++) {
                if (k > 0) printf(", ");
                print_json_string(a->args[k]);
            }
            printf("],");
            printf("\n        \"type\": ");
            print_json_string(a->type);
            printf("\n      }");
        }
        if (docs[i].aliases_count > 0) {
            printf("\n    ");
        }
        printf("],\n");

        /* Values */
        printf("    \"values\": [");
        for (int j = 0; j < docs[i].values_count; j++) {
            ElmValue *v = &docs[i].values[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(v->name);
            printf(",\n        \"comment\": ");
            print_json_string(v->comment);
            printf(",\n        \"type\": ");
            print_json_string(v->type);
            printf("\n      }");
        }
        if (docs[i].values_count > 0) {
            printf("\n    ");
        }
        printf("],\n");

        /* Binops */
        printf("    \"binops\": [");
        for (int j = 0; j < docs[i].binops_count; j++) {
            ElmBinop *b = &docs[i].binops[j];
            printf("%s\n      {", j > 0 ? "," : "");
            printf("\n        \"name\": ");
            print_json_string(b->name);
            printf(",\n        \"comment\": ");
            print_json_string(b->comment);
            printf(",\n        \"type\": ");
            print_json_string(b->type);
            printf(",\n        \"associativity\": ");
            print_json_string(b->associativity);
            printf(",\n        \"precedence\": %d", b->precedence);
            printf("\n      }");
        }
        if (docs[i].binops_count > 0) {
            printf("\n    ");
        }
        printf("]\n");

        printf("  }%s\n", i < docs_count - 1 ? "," : "");
    }

    printf("]\n");
}
