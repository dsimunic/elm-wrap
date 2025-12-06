#ifndef MODULE_PARSE_H
#define MODULE_PARSE_H

#include "tree_sitter/api.h"
#include "type_maps.h"

/* Helper function to extract module name and exports */
char *extract_module_info(TSNode root, const char *source_code, ExportList *exports);

/* Helper function to parse imports */
void extract_imports(TSNode root, const char *source_code, ImportMap *import_map, 
                     ModuleAliasMap *alias_map, DirectModuleImports *direct_imports, 
                     DependencyCache *dep_cache);

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
 */
void apply_implicit_imports(ImportMap *import_map, ModuleAliasMap *alias_map, 
                            DirectModuleImports *direct_imports, DependencyCache *dep_cache);

#endif /* MODULE_PARSE_H */
