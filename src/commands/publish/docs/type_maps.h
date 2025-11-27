#ifndef TYPE_MAPS_H
#define TYPE_MAPS_H

#include <stdbool.h>
#include "dependency_cache.h"

/* Import tracking - maps type names to their source modules */
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

/* ImportMap functions */
void init_import_map(ImportMap *map);
void add_import(ImportMap *map, const char *type_name, const char *module_name);
const char *lookup_import(ImportMap *map, const char *type_name);
void free_import_map(ImportMap *map);

/* DirectModuleImports functions */
void init_direct_imports(DirectModuleImports *imports);
void add_direct_import(DirectModuleImports *imports, const char *module_name);
void remove_direct_import(DirectModuleImports *imports, const char *module_name);
bool is_directly_imported(DirectModuleImports *imports, const char *module_name);
void free_direct_imports(DirectModuleImports *imports);

/* ModuleAliasMap functions */
void init_module_alias_map(ModuleAliasMap *map);
void add_module_alias(ModuleAliasMap *map, const char *alias, const char *full_module);
const char *lookup_module_alias(ModuleAliasMap *map, const char *alias,
                                const char *referenced_type,
                                DependencyCache *dep_cache,
                                bool *is_ambiguous,
                                const char **ambiguous_module1,
                                const char **ambiguous_module2);
void free_module_alias_map(ModuleAliasMap *map);

/* TypeAliasMap functions */
void init_type_alias_map(TypeAliasMap *map);
void add_type_alias(TypeAliasMap *map, const char *type_name, char **type_vars, int type_vars_count, const char *expansion);
const TypeAliasExpansion *lookup_type_alias(TypeAliasMap *map, const char *type_name);
void free_type_alias_map(TypeAliasMap *map);

/* ExportList functions */
void free_export_list(ExportList *exports);
bool is_exported_value(const char *name, ExportList *exports);
bool is_exported_type(const char *name, ExportList *exports);
bool is_type_exposed_with_constructors(const char *name, ExportList *exports);

#endif /* TYPE_MAPS_H */
