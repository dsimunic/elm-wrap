#ifndef TYPE_QUALIFY_H
#define TYPE_QUALIFY_H

#include "type_maps.h"
#include <stdbool.h>
#include <stddef.h>

/* Helper function to normalize whitespace - convert newlines and multiple spaces to single spaces */
char *normalize_whitespace(const char *str);

/* Helper function to parse a single type argument from a string
 * Returns the end position of the argument, or NULL if no valid argument found
 * Handles: simple identifiers, qualified types, parenthesized types, record types, tuple types */
const char *parse_type_arg(const char *start, char *out_arg, size_t out_size);

/* Helper function to substitute type variables with type arguments in an expansion string */
char *substitute_type_vars(const char *expansion, char **type_vars, int type_vars_count,
                           char **type_args, int type_args_count);

/* Helper function to check if a string contains a function arrow */
bool contains_function_arrow(const char *str);

/* Helper function to expand type aliases that are function types
 * Only expands the final return type, not parameter types
 * Only expands if implementation has more params than type arrows suggest */
char *expand_function_type_aliases(const char *type_str, TypeAliasMap *type_alias_map, int implementation_param_count);

/* Helper function to qualify type names based on import map and local types */
char *qualify_type_names(const char *type_str, const char *module_name,
                         ImportMap *import_map, ModuleAliasMap *alias_map,
                         DirectModuleImports *direct_imports,
                         char **local_types, int local_types_count,
                         DependencyCache *dep_cache);

/* Helper function to remove unnecessary outer parentheses from return type
 * Example: "A -> B -> (C -> D)" becomes "A -> B -> C -> D"
 * This matches Elm's canonical documentation format */
char *remove_return_type_parens(const char *type_str);

/* Helper function to remove unnecessary parentheses from function argument positions.
 * Example: "a -> (Maybe.Maybe b) -> Result.Result a ()" 
 *    becomes: "a -> Maybe.Maybe b -> Result.Result a ()"
 * Keeps parens that contain function arrows, commas (tuples), or are empty (unit type). */
char *remove_unnecessary_arg_parens(const char *type_str);

#endif /* TYPE_QUALIFY_H */
