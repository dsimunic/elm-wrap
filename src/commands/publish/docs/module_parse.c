#include "module_parse.h"
#include "tree_util.h"
#include "../../../alloc.h"
#include <string.h>

/* Forward declaration */
static void apply_wellknown_module_exports(ImportMap *import_map, const char *module_name);

/* Helper function to extract module name and exports */
char *extract_module_info(TSNode root, const char *source_code, ExportList *exports) {
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
                        } else if (strcmp(exp_type, "exposed_operator") == 0) {
                            /* Exposed operator (e.g., (|=), (|.)) */
                            TSNode operator_node = ts_node_child_by_field_name(exp_child, "operator", 8);
                            if (!ts_node_is_null(operator_node)) {
                                char *operator_name = get_node_text(operator_node, source_code);
                                exports->exposed_values = arena_realloc(exports->exposed_values,
                                    (exports->exposed_values_count + 1) * sizeof(char*));
                                exports->exposed_values[exports->exposed_values_count++] = operator_name;
                            }
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

/* Helper function to parse imports */
void extract_imports(TSNode root, const char *source_code, ImportMap *import_map, 
                     ModuleAliasMap *alias_map, DirectModuleImports *direct_imports, 
                     DependencyCache *dep_cache) {
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
             * - If alias name matches a direct import of a DIFFERENT module, create ambiguity
             *   for type-based resolution (e.g., "import WebGL" + "import WebGL.Matrices as WebGL")
             * Note: Aliases and direct imports can coexist for the same module */
            if (module_name && direct_imports) {
                if (!has_as_clause) {
                    /* Direct import: module is now available by its original name */
                    add_direct_import(direct_imports, module_name);
                } else if (module_alias) {
                    /* Aliased import: only remove from direct imports if it's the SAME module
                     * being re-imported with an alias. If it's a DIFFERENT module with the same
                     * name, don't remove - this creates an ambiguity resolved by type checking.
                     *
                     * Example 1: "import Html" then "import Html as H"
                     *   -> Remove "Html" from direct imports (same module)
                     *
                     * Example 2: "import WebGL" then "import WebGL.Matrices as WebGL"
                     *   -> Don't remove "WebGL" (different modules - WebGL vs WebGL.Matrices)
                     *   -> Will be resolved by checking which exports the referenced type */
                    if (is_directly_imported(direct_imports, module_alias) &&
                        strcmp(module_alias, module_name) == 0) {
                        /* Same module being re-imported with alias - remove redundant direct import */
                        remove_direct_import(direct_imports, module_alias);
                    }
                    /* If different modules with same name, don't remove - creates ambiguity for type resolution */
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

/* Apply Elm's implicit imports */
void apply_implicit_imports(ImportMap *import_map, ModuleAliasMap *alias_map, 
                            DirectModuleImports *direct_imports, DependencyCache *dep_cache) {
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
