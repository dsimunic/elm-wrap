#include "decl_extract.h"
#include "tree_util.h"
#include "type_qualify.h"
#include "comment_extract.h"
#include "../../../alloc.h"
#include "../../../ast/qualify.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Helper function to extract and canonicalize type expression */
char *extract_type_expression(TSNode type_node, const char *source_code, const char *module_name,
                              ImportMap *import_map, ModuleAliasMap *alias_map,
                              DirectModuleImports *direct_imports,
                              char **local_types, int local_types_count,
                              TypeAliasMap *type_alias_map, int implementation_param_count,
                              DependencyCache *dep_cache) {
    (void)type_alias_map;  /* Type alias expansion is intentionally skipped - we preserve aliases */
    (void)implementation_param_count;  /* Not needed when preserving aliases */

    if (ts_node_is_null(type_node)) {
        return arena_strdup("");
    }

    /* Build a QualifyContext from the existing maps */
    QualifyContext *ctx = qualify_context_create_from_maps(
        module_name, import_map, alias_map, direct_imports,
        local_types, local_types_count, dep_cache);

    /* Use AST-based qualification + canonicalization */
    char *result = qualify_and_canonicalize_type_node(type_node, source_code, ctx);

    qualify_context_free(ctx);

    return result;
}

/* Extract value declaration (function/constant) */
bool extract_value_decl(TSNode node, const char *source_code, ElmValue *value, const char *module_name,
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
bool extract_type_alias(TSNode node, const char *source_code, ElmAlias *alias, const char *module_name,
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
bool extract_union_type(TSNode node, const char *source_code, ElmUnion *union_type, const char *module_name,
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

/* Extract binop (infix operator) */
bool extract_binop(TSNode node, const char *source_code, ElmBinop *binop, const char *module_name,
                   ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                   char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                   DependencyCache *dep_cache) {
    /* Extract operator name using field name */
    TSNode operator_node = ts_node_child_by_field_name(node, "operator", 8);
    if (ts_node_is_null(operator_node)) {
        return false;
    }
    char *operator_name = get_node_text(operator_node, source_code);

    /* Extract associativity using field name */
    TSNode assoc_node = ts_node_child_by_field_name(node, "associativity", 13);
    if (ts_node_is_null(assoc_node)) {
        arena_free(operator_name);
        return false;
    }
    char *associativity = get_node_text(assoc_node, source_code);

    /* Extract precedence using field name */
    TSNode prec_node = ts_node_child_by_field_name(node, "precedence", 10);
    if (ts_node_is_null(prec_node)) {
        arena_free(operator_name);
        arena_free(associativity);
        return false;
    }
    char *precedence_str = get_node_text(prec_node, source_code);
    int precedence = atoi(precedence_str);
    arena_free(precedence_str);

    /* Find the value_expr child to get the function name */
    char *func_name = NULL;
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        if (strcmp(ts_node_type(child), "value_expr") == 0) {
            /* Get the function name from value_expr */
            func_name = get_node_text(child, source_code);
            break;
        }
    }

    if (!func_name) {
        arena_free(operator_name);
        arena_free(associativity);
        return false;
    }

    /* Now we need to find the type annotation for this function
     * Scan the parent (file_declaration list) for type_annotation with matching name */
    TSNode parent = ts_node_parent(node);
    char *type_str = NULL;
    TSNode type_annotation_node = {0}; /* Will hold the type annotation node for comment extraction */

    if (!ts_node_is_null(parent)) {
        uint32_t parent_child_count = ts_node_child_count(parent);
        for (uint32_t i = 0; i < parent_child_count; i++) {
            TSNode sibling = ts_node_child(parent, i);
            if (strcmp(ts_node_type(sibling), "type_annotation") == 0) {
                /* Check if this type annotation is for our function */
                uint32_t ann_child_count = ts_node_child_count(sibling);
                for (uint32_t j = 0; j < ann_child_count; j++) {
                    TSNode ann_child = ts_node_child(sibling, j);
                    if (strcmp(ts_node_type(ann_child), "lower_case_identifier") == 0) {
                        char *ann_name = get_node_text(ann_child, source_code);
                        bool match = (strcmp(ann_name, func_name) == 0);
                        arena_free(ann_name);

                        if (match) {
                            /* Found the matching type annotation, save it for comment extraction */
                            type_annotation_node = sibling;

                            /* Extract the type */
                            for (uint32_t k = 0; k < ann_child_count; k++) {
                                TSNode type_child = ts_node_child(sibling, k);
                                if (strcmp(ts_node_type(type_child), "type_expression") == 0) {
                                    /* Count implementation parameters by finding the value_declaration */
                                    int impl_param_count = 0;
                                    for (uint32_t m = 0; m < parent_child_count; m++) {
                                        TSNode decl = ts_node_child(parent, m);
                                        if (strcmp(ts_node_type(decl), "value_declaration") == 0) {
                                            impl_param_count = count_implementation_params(decl, source_code);
                                            break;
                                        }
                                    }

                                    type_str = extract_type_expression(type_child, source_code, module_name,
                                                                       import_map, alias_map, direct_imports,
                                                                       local_types, local_types_count,
                                                                       type_alias_map, impl_param_count, dep_cache);
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                if (type_str) break;
            }
        }
    }

    /* If we couldn't find the type, that's an error */
    if (!type_str) {
        arena_free(operator_name);
        arena_free(associativity);
        arena_free(func_name);
        return false;
    }

    /* Extract comment from the function's type annotation, not the infix declaration */
    char *comment = NULL;
    if (!ts_node_is_null(type_annotation_node)) {
        comment = find_preceding_comment(type_annotation_node, parent, source_code);
    }
    if (!comment) {
        comment = arena_strdup("");
    }

    /* Clean up func_name as we don't need it anymore */
    arena_free(func_name);

    /* Set the binop fields */
    binop->name = operator_name;
    binop->comment = comment;
    binop->type = type_str;
    binop->precedence = precedence;
    binop->associativity = associativity;

    return true;
}
