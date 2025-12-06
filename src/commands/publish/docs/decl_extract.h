#ifndef DECL_EXTRACT_H
#define DECL_EXTRACT_H

#include "tree_sitter/api.h"
#include <stdbool.h>
#include "elm_docs.h"
#include "type_maps.h"

/* Helper function to extract and canonicalize type expression */
char *extract_type_expression(TSNode type_node, const char *source_code, const char *module_name,
                              ImportMap *import_map, ModuleAliasMap *alias_map,
                              DirectModuleImports *direct_imports,
                              char **local_types, int local_types_count,
                              TypeAliasMap *type_alias_map, int implementation_param_count,
                              DependencyCache *dep_cache);

/* Extract value declaration (function/constant) */
bool extract_value_decl(TSNode node, const char *source_code, ElmValue *value, const char *module_name,
                        ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                        char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                        DependencyCache *dep_cache);

/* Extract type alias */
bool extract_type_alias(TSNode node, const char *source_code, ElmAlias *alias, const char *module_name,
                        ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                        char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                        DependencyCache *dep_cache);

/* Extract union type */
bool extract_union_type(TSNode node, const char *source_code, ElmUnion *union_type, const char *module_name,
                        ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                        char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                        DependencyCache *dep_cache);

/* Extract binop (infix operator) */
bool extract_binop(TSNode node, const char *source_code, ElmBinop *binop, const char *module_name,
                   ImportMap *import_map, ModuleAliasMap *alias_map, DirectModuleImports *direct_imports,
                   char **local_types, int local_types_count, TypeAliasMap *type_alias_map,
                   DependencyCache *dep_cache);

#endif /* DECL_EXTRACT_H */
