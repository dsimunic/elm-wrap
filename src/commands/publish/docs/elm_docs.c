/**
 * elm_docs.c - Main Elm documentation parser
 *
 * This file orchestrates the parsing of Elm source files to generate
 * documentation in the docs.json format used by Elm packages.
 *
 * The heavy lifting is delegated to specialized modules:
 *   - type_maps: Data structures for tracking imports, exports, aliases
 *   - tree_util: Tree-sitter utilities and text extraction
 *   - comment_extract: Documentation comment extraction
 *   - type_qualify: Type name qualification and normalization
 *   - module_parse: Module declaration and import parsing
 *   - decl_extract: Declaration extraction (values, aliases, unions)
 *   - docs_json: JSON output generation
 */

#include "elm_docs.h"
#include "type_maps.h"
#include "tree_util.h"
#include "comment_extract.h"
#include "type_qualify.h"
#include "module_parse.h"
#include "decl_extract.h"
#include "docs_json.h"
#include "dependency_cache.h"
#include "../../../alloc.h"

#include <tree_sitter/api.h>
#include <stdio.h>
#include <string.h>

/* External tree-sitter Elm parser */
extern const TSLanguage *tree_sitter_elm(void);

/* Main parsing function */
bool parse_elm_file(const char *filepath, ElmModuleDocs *docs, DependencyCache *dep_cache) {
    /* Initialize the docs structure */
    memset(docs, 0, sizeof(ElmModuleDocs));

    /* Read file content */
    char *source_code = read_file_normalized(filepath);
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
    int binops_capacity = 4;

    docs->values = arena_malloc(values_capacity * sizeof(ElmValue));
    docs->aliases = arena_malloc(aliases_capacity * sizeof(ElmAlias));
    docs->unions = arena_malloc(unions_capacity * sizeof(ElmUnion));
    docs->binops = arena_malloc(binops_capacity * sizeof(ElmBinop));
    docs->values_count = 0;
    docs->aliases_count = 0;
    docs->unions_count = 0;
    docs->binops_count = 0;

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
            int type_vars_capacity = 8;
            char **type_vars = arena_malloc(type_vars_capacity * sizeof(char*));
            int type_vars_count = 0;
            char *expansion = NULL;

            uint32_t type_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < type_child_count; j++) {
                TSNode type_child = ts_node_child(child, j);
                const char *child_type = ts_node_type(type_child);

                if (strcmp(child_type, "upper_case_identifier") == 0 && !alias_name) {
                    alias_name = get_node_text(type_child, source_code);
                } else if (strcmp(child_type, "lower_type_name") == 0) {
                    if (type_vars_count >= type_vars_capacity) {
                        type_vars_capacity *= 2;
                        type_vars = arena_realloc(type_vars, type_vars_capacity * sizeof(char*));
                    }
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
        } else if (strcmp(type, "infix_declaration") == 0) {
            /* Found an infix operator declaration */
            ElmBinop binop;
            if (extract_binop(child, source_code, &binop, docs->name, &import_map, &alias_map, &direct_imports, local_types, local_types_count, &type_alias_map, dep_cache)) {
                /* Only include if exported (operators are exported like values) */
                if (is_exported_value(binop.name, &exports)) {
                    if (docs->binops_count >= binops_capacity) {
                        binops_capacity *= 2;
                        docs->binops = arena_realloc(docs->binops, binops_capacity * sizeof(ElmBinop));
                    }
                    docs->binops[docs->binops_count++] = binop;
                } else {
                    /* Not exported, free the binop */
                    arena_free(binop.name);
                    arena_free(binop.comment);
                    arena_free(binop.type);
                    arena_free(binop.associativity);
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

    fprintf(stderr, "Successfully parsed: %s (Module: %s, %d values, %d aliases, %d unions, %d binops)\n",
            filepath, docs->name, docs->values_count, docs->aliases_count, docs->unions_count, docs->binops_count);

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
