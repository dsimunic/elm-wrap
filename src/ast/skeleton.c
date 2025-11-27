/**
 * skeleton.c - Skeleton AST parsing implementation
 *
 * Parses an Elm source file into a skeleton containing only the structural
 * information needed for documentation generation.
 */

#include "skeleton.h"
#include "util.h"
#include "../alloc.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal helpers - forward declarations
 * ========================================================================== */

static void parse_module_declaration(TSNode node, const char *source, SkeletonModule *mod);
static void parse_import_declaration(TSNode node, const char *source, SkeletonModule *mod);
static void parse_type_annotation(TSNode node, TSNode value_decl, const char *source, SkeletonModule *mod);
static void parse_type_alias_declaration(TSNode node, const char *source, SkeletonModule *mod);
static void parse_type_declaration(TSNode node, const char *source, SkeletonModule *mod);
static void parse_infix_declaration(TSNode node, const char *source, SkeletonModule *mod);
static char *find_preceding_doc_comment(TSNode node, TSNode parent, const char *source);
static int count_implementation_params(TSNode value_decl, const char *source);

/* ============================================================================
 * Lifecycle functions
 * ========================================================================== */

SkeletonModule *skeleton_parse(const char *filepath) {
    char *source = ast_read_file_normalized(filepath);
    if (!source) {
        return NULL;
    }

    SkeletonModule *mod = skeleton_parse_string(source, filepath);
    if (!mod) {
        arena_free(source);
        return NULL;
    }

    /* skeleton_parse_string makes its own copy, but we reuse the source */
    /* Actually, let's transfer ownership */
    arena_free(mod->source_code);
    mod->source_code = source;

    return mod;
}

SkeletonModule *skeleton_parse_string(const char *source_code, const char *filepath) {
    TSParser *parser = ast_create_elm_parser();
    if (!parser) {
        return NULL;
    }

    TSTree *tree = ts_parser_parse_string(parser, NULL, source_code, strlen(source_code));
    if (!tree) {
        ts_parser_delete(parser);
        return NULL;
    }

    /* Allocate and initialize module */
    SkeletonModule *mod = arena_calloc(1, sizeof(SkeletonModule));
    mod->filepath = arena_strdup(filepath ? filepath : "<string>");
    mod->source_code = arena_strdup(source_code);
    mod->tree = tree;

    /* Initialize dynamic arrays */
    mod->imports_capacity = 16;
    mod->imports = arena_malloc(mod->imports_capacity * sizeof(SkeletonImport));

    mod->type_annotations_capacity = 32;
    mod->type_annotations = arena_malloc(mod->type_annotations_capacity * sizeof(SkeletonTypeAnnotation));

    mod->type_aliases_capacity = 16;
    mod->type_aliases = arena_malloc(mod->type_aliases_capacity * sizeof(SkeletonTypeAlias));

    mod->union_types_capacity = 16;
    mod->union_types = arena_malloc(mod->union_types_capacity * sizeof(SkeletonUnionType));

    mod->infixes_capacity = 8;
    mod->infixes = arena_malloc(mod->infixes_capacity * sizeof(SkeletonInfix));

    mod->local_types_capacity = 16;
    mod->local_types = arena_malloc(mod->local_types_capacity * sizeof(char*));

    mod->exports.values_capacity = 32;
    mod->exports.values = arena_malloc(mod->exports.values_capacity * sizeof(char*));
    mod->exports.types_capacity = 16;
    mod->exports.types = arena_malloc(mod->exports.types_capacity * sizeof(char*));
    mod->exports.types_with_constructors_capacity = 16;
    mod->exports.types_with_constructors = arena_malloc(
        mod->exports.types_with_constructors_capacity * sizeof(char*));

    /* Walk the AST and extract skeleton */
    TSNode root = ts_tree_root_node(tree);
    uint32_t child_count = ts_node_child_count(root);

    /* First pass: module declaration and collect local type names */
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "module_declaration") == 0) {
            parse_module_declaration(child, source_code, mod);

            /* Look for module doc comment */
            for (uint32_t j = i + 1; j < child_count; j++) {
                TSNode next = ts_node_child(root, j);
                const char *next_type = ts_node_type(next);
                if (strcmp(next_type, "block_comment") == 0) {
                    char *raw = ast_get_node_text(next, source_code);
                    /* TODO: clean_comment - for now store raw */
                    mod->module_doc_comment = raw;
                    break;
                }
                if (strcmp(next_type, "value_declaration") == 0 ||
                    strcmp(next_type, "type_alias_declaration") == 0 ||
                    strcmp(next_type, "type_declaration") == 0 ||
                    strcmp(next_type, "import_clause") == 0) {
                    break;
                }
            }
        } else if (strcmp(type, "type_alias_declaration") == 0 ||
                   strcmp(type, "type_declaration") == 0) {
            /* Extract type name for local types list */
            uint32_t type_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < type_child_count; j++) {
                TSNode type_child = ts_node_child(child, j);
                if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                    char *name = ast_get_node_text(type_child, source_code);
                    if (mod->local_types_count >= mod->local_types_capacity) {
                        mod->local_types_capacity *= 2;
                        mod->local_types = arena_realloc(mod->local_types,
                            mod->local_types_capacity * sizeof(char*));
                    }
                    mod->local_types[mod->local_types_count++] = name;
                    break;
                }
            }
        }
    }

    /* Second pass: imports and declarations */
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "import_clause") == 0) {
            parse_import_declaration(child, source_code, mod);
        } else if (strcmp(type, "value_declaration") == 0) {
            /* Look for preceding type_annotation */
            TSNode prev = ts_node_prev_named_sibling(child);
            if (!ts_node_is_null(prev) && strcmp(ts_node_type(prev), "type_annotation") == 0) {
                parse_type_annotation(prev, child, source_code, mod);
            }
        } else if (strcmp(type, "type_alias_declaration") == 0) {
            parse_type_alias_declaration(child, source_code, mod);
        } else if (strcmp(type, "type_declaration") == 0) {
            parse_type_declaration(child, source_code, mod);
        } else if (strcmp(type, "infix_declaration") == 0) {
            parse_infix_declaration(child, source_code, mod);
        }
    }

    ts_parser_delete(parser);
    return mod;
}

void skeleton_free(SkeletonModule *mod) {
    if (!mod) return;

    arena_free(mod->filepath);
    arena_free(mod->source_code);
    if (mod->tree) {
        ts_tree_delete(mod->tree);
    }
    arena_free(mod->module_name);
    arena_free(mod->module_doc_comment);

    /* Free exports */
    for (int i = 0; i < mod->exports.values_count; i++) {
        arena_free(mod->exports.values[i]);
    }
    arena_free(mod->exports.values);
    for (int i = 0; i < mod->exports.types_count; i++) {
        arena_free(mod->exports.types[i]);
    }
    arena_free(mod->exports.types);
    for (int i = 0; i < mod->exports.types_with_constructors_count; i++) {
        arena_free(mod->exports.types_with_constructors[i]);
    }
    arena_free(mod->exports.types_with_constructors);

    /* Free imports */
    for (int i = 0; i < mod->imports_count; i++) {
        SkeletonImport *imp = &mod->imports[i];
        arena_free(imp->module_name);
        arena_free(imp->alias);
        for (int j = 0; j < imp->exposed_values_count; j++) {
            arena_free(imp->exposed_values[j]);
        }
        arena_free(imp->exposed_values);
        for (int j = 0; j < imp->exposed_types_count; j++) {
            arena_free(imp->exposed_types[j]);
        }
        arena_free(imp->exposed_types);
        for (int j = 0; j < imp->exposed_types_with_constructors_count; j++) {
            arena_free(imp->exposed_types_with_constructors[j]);
        }
        arena_free(imp->exposed_types_with_constructors);
    }
    arena_free(mod->imports);

    /* Free type annotations */
    for (int i = 0; i < mod->type_annotations_count; i++) {
        SkeletonTypeAnnotation *ann = &mod->type_annotations[i];
        arena_free(ann->name);
        arena_free(ann->doc_comment);
        arena_free(ann->qualified_type);
        arena_free(ann->canonical_type);
    }
    arena_free(mod->type_annotations);

    /* Free type aliases */
    for (int i = 0; i < mod->type_aliases_count; i++) {
        SkeletonTypeAlias *alias = &mod->type_aliases[i];
        arena_free(alias->name);
        for (int j = 0; j < alias->type_params_count; j++) {
            arena_free(alias->type_params[j]);
        }
        arena_free(alias->type_params);
        arena_free(alias->doc_comment);
        arena_free(alias->qualified_type);
        arena_free(alias->canonical_type);
    }
    arena_free(mod->type_aliases);

    /* Free union types */
    for (int i = 0; i < mod->union_types_count; i++) {
        SkeletonUnionType *ut = &mod->union_types[i];
        arena_free(ut->name);
        for (int j = 0; j < ut->type_params_count; j++) {
            arena_free(ut->type_params[j]);
        }
        arena_free(ut->type_params);
        arena_free(ut->doc_comment);
        for (int j = 0; j < ut->constructors_count; j++) {
            SkeletonUnionConstructor *ctor = &ut->constructors[j];
            arena_free(ctor->name);
            arena_free(ctor->arg_nodes);
            for (int k = 0; k < ctor->arg_nodes_count; k++) {
                arena_free(ctor->qualified_args[k]);
                arena_free(ctor->canonical_args[k]);
            }
            arena_free(ctor->qualified_args);
            arena_free(ctor->canonical_args);
        }
        arena_free(ut->constructors);
    }
    arena_free(mod->union_types);

    /* Free infixes */
    for (int i = 0; i < mod->infixes_count; i++) {
        SkeletonInfix *inf = &mod->infixes[i];
        arena_free(inf->operator);
        arena_free(inf->function_name);
        arena_free(inf->associativity);
    }
    arena_free(mod->infixes);

    /* Free local types */
    for (int i = 0; i < mod->local_types_count; i++) {
        arena_free(mod->local_types[i]);
    }
    arena_free(mod->local_types);

    arena_free(mod);
}

/* ============================================================================
 * Query functions
 * ========================================================================== */

bool skeleton_is_value_exported(const SkeletonModule *mod, const char *name) {
    if (mod->exports.expose_all) return true;

    for (int i = 0; i < mod->exports.values_count; i++) {
        if (strcmp(mod->exports.values[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool skeleton_is_type_exported(const SkeletonModule *mod, const char *name) {
    if (mod->exports.expose_all) return true;

    for (int i = 0; i < mod->exports.types_count; i++) {
        if (strcmp(mod->exports.types[i], name) == 0) {
            return true;
        }
    }
    for (int i = 0; i < mod->exports.types_with_constructors_count; i++) {
        if (strcmp(mod->exports.types_with_constructors[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool skeleton_is_type_exposed_with_constructors(const SkeletonModule *mod, const char *name) {
    if (mod->exports.expose_all) return true;

    for (int i = 0; i < mod->exports.types_with_constructors_count; i++) {
        if (strcmp(mod->exports.types_with_constructors[i], name) == 0) {
            return true;
        }
    }
    return false;
}

SkeletonTypeAnnotation *skeleton_find_type_annotation(SkeletonModule *mod, const char *name) {
    for (int i = 0; i < mod->type_annotations_count; i++) {
        if (strcmp(mod->type_annotations[i].name, name) == 0) {
            return &mod->type_annotations[i];
        }
    }
    return NULL;
}

SkeletonTypeAlias *skeleton_find_type_alias(SkeletonModule *mod, const char *name) {
    for (int i = 0; i < mod->type_aliases_count; i++) {
        if (strcmp(mod->type_aliases[i].name, name) == 0) {
            return &mod->type_aliases[i];
        }
    }
    return NULL;
}

SkeletonUnionType *skeleton_find_union_type(SkeletonModule *mod, const char *name) {
    for (int i = 0; i < mod->union_types_count; i++) {
        if (strcmp(mod->union_types[i].name, name) == 0) {
            return &mod->union_types[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * Internal parsing helpers
 * ========================================================================== */

static void parse_module_declaration(TSNode node, const char *source, SkeletonModule *mod) {
    uint32_t child_count = ts_node_child_count(node);

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "upper_case_qid") == 0) {
            mod->module_name = ast_get_node_text(child, source);
        } else if (strcmp(type, "exposing_list") == 0) {
            /* Parse exposing list */
            uint32_t exp_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < exp_child_count; j++) {
                TSNode exp_child = ts_node_child(child, j);
                const char *exp_type = ts_node_type(exp_child);

                if (strcmp(exp_type, "double_dot") == 0) {
                    mod->exports.expose_all = true;
                } else if (strcmp(exp_type, "exposed_value") == 0) {
                    char *name = ast_get_node_text(exp_child, source);
                    if (mod->exports.values_count >= mod->exports.values_capacity) {
                        mod->exports.values_capacity *= 2;
                        mod->exports.values = arena_realloc(mod->exports.values,
                            mod->exports.values_capacity * sizeof(char*));
                    }
                    mod->exports.values[mod->exports.values_count++] = name;
                } else if (strcmp(exp_type, "exposed_type") == 0) {
                    /* Check for (..) */
                    bool has_constructors = false;
                    char *type_name = NULL;
                    uint32_t et_child_count = ts_node_child_count(exp_child);
                    for (uint32_t k = 0; k < et_child_count; k++) {
                        TSNode et_child = ts_node_child(exp_child, k);
                        const char *et_type = ts_node_type(et_child);
                        if (strcmp(et_type, "upper_case_identifier") == 0) {
                            type_name = ast_get_node_text(et_child, source);
                        } else if (strcmp(et_type, "exposed_union_constructors") == 0) {
                            has_constructors = true;
                        }
                    }

                    if (type_name) {
                        if (has_constructors) {
                            if (mod->exports.types_with_constructors_count >=
                                mod->exports.types_with_constructors_capacity) {
                                mod->exports.types_with_constructors_capacity *= 2;
                                mod->exports.types_with_constructors = arena_realloc(
                                    mod->exports.types_with_constructors,
                                    mod->exports.types_with_constructors_capacity * sizeof(char*));
                            }
                            mod->exports.types_with_constructors[
                                mod->exports.types_with_constructors_count++] = type_name;
                        } else {
                            if (mod->exports.types_count >= mod->exports.types_capacity) {
                                mod->exports.types_capacity *= 2;
                                mod->exports.types = arena_realloc(mod->exports.types,
                                    mod->exports.types_capacity * sizeof(char*));
                            }
                            mod->exports.types[mod->exports.types_count++] = type_name;
                        }
                    }
                } else if (strcmp(exp_type, "exposed_operator") == 0) {
                    /* Operators go to values */
                    char *name = ast_get_node_text(exp_child, source);
                    if (mod->exports.values_count >= mod->exports.values_capacity) {
                        mod->exports.values_capacity *= 2;
                        mod->exports.values = arena_realloc(mod->exports.values,
                            mod->exports.values_capacity * sizeof(char*));
                    }
                    mod->exports.values[mod->exports.values_count++] = name;
                }
            }
        }
    }
}

static void parse_import_declaration(TSNode node, const char *source, SkeletonModule *mod) {
    if (mod->imports_count >= mod->imports_capacity) {
        mod->imports_capacity *= 2;
        mod->imports = arena_realloc(mod->imports, mod->imports_capacity * sizeof(SkeletonImport));
    }

    SkeletonImport *imp = &mod->imports[mod->imports_count++];
    memset(imp, 0, sizeof(SkeletonImport));

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "upper_case_qid") == 0) {
            imp->module_name = ast_get_node_text(child, source);
        } else if (strcmp(type, "as_clause") == 0) {
            TSNode alias_id = ast_find_child_by_type(child, "upper_case_identifier");
            if (!ts_node_is_null(alias_id)) {
                imp->alias = ast_get_node_text(alias_id, source);
            }
        } else if (strcmp(type, "exposing_list") == 0) {
            /* Parse exposing */
            imp->exposed_values = arena_malloc(16 * sizeof(char*));
            imp->exposed_types = arena_malloc(16 * sizeof(char*));
            imp->exposed_types_with_constructors = arena_malloc(16 * sizeof(char*));

            uint32_t exp_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < exp_count; j++) {
                TSNode exp = ts_node_child(child, j);
                const char *exp_type = ts_node_type(exp);

                if (strcmp(exp_type, "double_dot") == 0) {
                    imp->expose_all = true;
                } else if (strcmp(exp_type, "exposed_value") == 0) {
                    imp->exposed_values[imp->exposed_values_count++] =
                        ast_get_node_text(exp, source);
                } else if (strcmp(exp_type, "exposed_type") == 0) {
                    bool has_ctors = false;
                    char *name = NULL;
                    uint32_t et_count = ts_node_child_count(exp);
                    for (uint32_t k = 0; k < et_count; k++) {
                        TSNode etc = ts_node_child(exp, k);
                        if (strcmp(ts_node_type(etc), "upper_case_identifier") == 0) {
                            name = ast_get_node_text(etc, source);
                        } else if (strcmp(ts_node_type(etc), "exposed_union_constructors") == 0) {
                            has_ctors = true;
                        }
                    }
                    if (name) {
                        if (has_ctors) {
                            imp->exposed_types_with_constructors[
                                imp->exposed_types_with_constructors_count++] = name;
                        } else {
                            imp->exposed_types[imp->exposed_types_count++] = name;
                        }
                    }
                } else if (strcmp(exp_type, "exposed_operator") == 0) {
                    imp->exposed_values[imp->exposed_values_count++] =
                        ast_get_node_text(exp, source);
                }
            }
        }
    }
}

static void parse_type_annotation(TSNode node, TSNode value_decl, const char *source, SkeletonModule *mod) {
    if (mod->type_annotations_count >= mod->type_annotations_capacity) {
        mod->type_annotations_capacity *= 2;
        mod->type_annotations = arena_realloc(mod->type_annotations,
            mod->type_annotations_capacity * sizeof(SkeletonTypeAnnotation));
    }

    SkeletonTypeAnnotation *ann = &mod->type_annotations[mod->type_annotations_count++];
    memset(ann, 0, sizeof(SkeletonTypeAnnotation));

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "lower_case_identifier") == 0 && !ann->name) {
            ann->name = ast_get_node_text(child, source);
        } else if (strcmp(type, "type_expression") == 0) {
            ann->type_node = child;
        }
    }

    ann->implementation_param_count = count_implementation_params(value_decl, source);
    ann->doc_comment = find_preceding_doc_comment(node, ts_node_parent(node), source);
}

static void parse_type_alias_declaration(TSNode node, const char *source, SkeletonModule *mod) {
    if (mod->type_aliases_count >= mod->type_aliases_capacity) {
        mod->type_aliases_capacity *= 2;
        mod->type_aliases = arena_realloc(mod->type_aliases,
            mod->type_aliases_capacity * sizeof(SkeletonTypeAlias));
    }

    SkeletonTypeAlias *alias = &mod->type_aliases[mod->type_aliases_count++];
    memset(alias, 0, sizeof(SkeletonTypeAlias));

    alias->type_params = arena_malloc(8 * sizeof(char*));

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "upper_case_identifier") == 0 && !alias->name) {
            alias->name = ast_get_node_text(child, source);
        } else if (strcmp(type, "lower_type_name") == 0) {
            alias->type_params[alias->type_params_count++] = ast_get_node_text(child, source);
        } else if (strcmp(type, "type_expression") == 0) {
            alias->type_node = child;
        }
    }

    alias->doc_comment = find_preceding_doc_comment(node, ts_node_parent(node), source);
}

static void parse_type_declaration(TSNode node, const char *source, SkeletonModule *mod) {
    if (mod->union_types_count >= mod->union_types_capacity) {
        mod->union_types_capacity *= 2;
        mod->union_types = arena_realloc(mod->union_types,
            mod->union_types_capacity * sizeof(SkeletonUnionType));
    }

    SkeletonUnionType *ut = &mod->union_types[mod->union_types_count++];
    memset(ut, 0, sizeof(SkeletonUnionType));

    ut->type_params = arena_malloc(8 * sizeof(char*));

    /* Count constructors first */
    int ctor_count = 0;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), "union_variant") == 0) {
            ctor_count++;
        }
    }

    ut->constructors = arena_malloc(ctor_count * sizeof(SkeletonUnionConstructor));

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "upper_case_identifier") == 0 && !ut->name) {
            ut->name = ast_get_node_text(child, source);
        } else if (strcmp(type, "lower_type_name") == 0) {
            ut->type_params[ut->type_params_count++] = ast_get_node_text(child, source);
        } else if (strcmp(type, "union_variant") == 0) {
            SkeletonUnionConstructor *ctor = &ut->constructors[ut->constructors_count++];
            memset(ctor, 0, sizeof(SkeletonUnionConstructor));

            /* Count args first */
            int arg_count = 0;
            uint32_t vc_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < vc_count; j++) {
                TSNode vc = ts_node_child(child, j);
                const char *vc_type = ts_node_type(vc);
                if (strcmp(vc_type, "type_expression") == 0 ||
                    strcmp(vc_type, "type_ref") == 0 ||
                    strcmp(vc_type, "record_type") == 0 ||
                    strcmp(vc_type, "tuple_type") == 0 ||
                    strcmp(vc_type, "type_variable") == 0) {
                    arg_count++;
                }
            }

            if (arg_count > 0) {
                ctor->arg_nodes = arena_malloc(arg_count * sizeof(TSNode));
            }

            for (uint32_t j = 0; j < vc_count; j++) {
                TSNode vc = ts_node_child(child, j);
                const char *vc_type = ts_node_type(vc);

                if (strcmp(vc_type, "upper_case_identifier") == 0 && !ctor->name) {
                    ctor->name = ast_get_node_text(vc, source);
                } else if (strcmp(vc_type, "type_expression") == 0 ||
                           strcmp(vc_type, "type_ref") == 0 ||
                           strcmp(vc_type, "record_type") == 0 ||
                           strcmp(vc_type, "tuple_type") == 0 ||
                           strcmp(vc_type, "type_variable") == 0) {
                    ctor->arg_nodes[ctor->arg_nodes_count++] = vc;
                }
            }
        }
    }

    ut->doc_comment = find_preceding_doc_comment(node, ts_node_parent(node), source);
}

static void parse_infix_declaration(TSNode node, const char *source, SkeletonModule *mod) {
    if (mod->infixes_count >= mod->infixes_capacity) {
        mod->infixes_capacity *= 2;
        mod->infixes = arena_realloc(mod->infixes, mod->infixes_capacity * sizeof(SkeletonInfix));
    }

    SkeletonInfix *inf = &mod->infixes[mod->infixes_count++];
    memset(inf, 0, sizeof(SkeletonInfix));

    /* Use field accessors */
    TSNode op_node = ts_node_child_by_field_name(node, "operator", 8);
    if (!ts_node_is_null(op_node)) {
        inf->operator = ast_get_node_text(op_node, source);
    }

    TSNode assoc_node = ts_node_child_by_field_name(node, "associativity", 13);
    if (!ts_node_is_null(assoc_node)) {
        inf->associativity = ast_get_node_text(assoc_node, source);
    }

    TSNode prec_node = ts_node_child_by_field_name(node, "precedence", 10);
    if (!ts_node_is_null(prec_node)) {
        char *prec_str = ast_get_node_text(prec_node, source);
        inf->precedence = atoi(prec_str);
        arena_free(prec_str);
    }

    /* Find function name from value_expr */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), "value_expr") == 0) {
            inf->function_name = ast_get_node_text(child, source);
            break;
        }
    }
}

static char *find_preceding_doc_comment(TSNode node, TSNode parent, const char *source) {
    if (ts_node_is_null(parent)) return NULL;

    TSNode prev = ts_node_prev_named_sibling(node);

    /* Skip any line comments to find block comment */
    while (!ts_node_is_null(prev)) {
        const char *type = ts_node_type(prev);
        if (strcmp(type, "block_comment") == 0) {
            char *text = ast_get_node_text(prev, source);
            /* Check if it's a doc comment (starts with {-|) */
            if (strncmp(text, "{-|", 3) == 0) {
                return text;
            }
            arena_free(text);
            return NULL;
        } else if (strcmp(type, "line_comment") == 0) {
            prev = ts_node_prev_named_sibling(prev);
        } else {
            break;
        }
    }

    return NULL;
}

static int count_implementation_params(TSNode value_decl, const char *source) {
    (void)source;
    int param_count = 0;

    uint32_t child_count = ts_node_child_count(value_decl);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(value_decl, i);

        if (strcmp(ts_node_type(child), "function_declaration_left") == 0) {
            uint32_t func_child_count = ts_node_child_count(child);
            bool found_func_name = false;
            for (uint32_t j = 0; j < func_child_count; j++) {
                TSNode func_child = ts_node_child(child, j);
                const char *func_child_type = ts_node_type(func_child);

                if (!found_func_name && strcmp(func_child_type, "lower_case_identifier") == 0) {
                    found_func_name = true;
                    continue;
                }

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
