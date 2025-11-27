#include "type_maps.h"
#include "../../../alloc.h"
#include <string.h>

/* ============================================================================
 * ImportMap functions
 * ============================================================================ */

void init_import_map(ImportMap *map) {
    map->imports = arena_malloc(32 * sizeof(TypeImport));
    map->imports_count = 0;
    map->imports_capacity = 32;
}

void add_import(ImportMap *map, const char *type_name, const char *module_name) {
    if (map->imports_count >= map->imports_capacity) {
        map->imports_capacity *= 2;
        map->imports = arena_realloc(map->imports, map->imports_capacity * sizeof(TypeImport));
    }
    map->imports[map->imports_count].type_name = arena_strdup(type_name);
    map->imports[map->imports_count].module_name = arena_strdup(module_name);
    map->imports_count++;
}

const char *lookup_import(ImportMap *map, const char *type_name) {
    /* Search backwards to implement "last import wins" semantics:
     * When the same type is exposed from multiple modules, the last import takes precedence */
    for (int i = map->imports_count - 1; i >= 0; i--) {
        if (strcmp(map->imports[i].type_name, type_name) == 0) {
            return map->imports[i].module_name;
        }
    }
    return NULL;
}

void free_import_map(ImportMap *map) {
    for (int i = 0; i < map->imports_count; i++) {
        arena_free(map->imports[i].type_name);
        arena_free(map->imports[i].module_name);
    }
    arena_free(map->imports);
}

/* ============================================================================
 * DirectModuleImports functions
 * ============================================================================ */

void init_direct_imports(DirectModuleImports *imports) {
    imports->modules = arena_malloc(16 * sizeof(char*));
    imports->modules_count = 0;
    imports->modules_capacity = 16;
}

void add_direct_import(DirectModuleImports *imports, const char *module_name) {
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
void remove_direct_import(DirectModuleImports *imports, const char *module_name) {
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

bool is_directly_imported(DirectModuleImports *imports, const char *module_name) {
    for (int i = 0; i < imports->modules_count; i++) {
        if (strcmp(imports->modules[i], module_name) == 0) {
            return true;
        }
    }
    return false;
}

void free_direct_imports(DirectModuleImports *imports) {
    for (int i = 0; i < imports->modules_count; i++) {
        arena_free(imports->modules[i]);
    }
    arena_free(imports->modules);
}

/* ============================================================================
 * ModuleAliasMap functions
 * ============================================================================ */

void init_module_alias_map(ModuleAliasMap *map) {
    map->aliases = arena_malloc(16 * sizeof(ModuleAlias));
    map->aliases_count = 0;
    map->aliases_capacity = 16;
}

void add_module_alias(ModuleAliasMap *map, const char *alias, const char *full_module) {
    /* Check if this alias already exists */
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].alias, alias) == 0) {
            /* Check if this exact module is already in the list */
            for (int j = 0; j < map->aliases[i].full_modules_count; j++) {
                if (strcmp(map->aliases[i].full_modules[j], full_module) == 0) {
                    /* Already have this exact mapping, nothing to do */
                    return;
                }
            }
            /* Different module with same alias - add it to the array */
            if (map->aliases[i].full_modules_count >= map->aliases[i].full_modules_capacity) {
                map->aliases[i].full_modules_capacity =
                    map->aliases[i].full_modules_capacity == 0 ? 4 : map->aliases[i].full_modules_capacity * 2;
                map->aliases[i].full_modules = arena_realloc(
                    map->aliases[i].full_modules,
                    map->aliases[i].full_modules_capacity * sizeof(char*)
                );
            }
            map->aliases[i].full_modules[map->aliases[i].full_modules_count++] = arena_strdup(full_module);
            return;
        }
    }

    /* New alias - create entry with first module */
    if (map->aliases_count >= map->aliases_capacity) {
        map->aliases_capacity *= 2;
        map->aliases = arena_realloc(map->aliases, map->aliases_capacity * sizeof(ModuleAlias));
    }
    map->aliases[map->aliases_count].alias = arena_strdup(alias);
    map->aliases[map->aliases_count].full_modules_capacity = 4;
    map->aliases[map->aliases_count].full_modules = arena_malloc(4 * sizeof(char*));
    map->aliases[map->aliases_count].full_modules[0] = arena_strdup(full_module);
    map->aliases[map->aliases_count].full_modules_count = 1;
    map->aliases_count++;
}

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

const char *lookup_module_alias(ModuleAliasMap *map, const char *alias,
                                const char *referenced_type,
                                DependencyCache *dep_cache,
                                bool *is_ambiguous,
                                const char **ambiguous_module1,
                                const char **ambiguous_module2) {
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].alias, alias) == 0) {
            int module_count = map->aliases[i].full_modules_count;

            /* If only one module uses this alias, return it immediately */
            if (module_count == 1) {
                if (is_ambiguous) *is_ambiguous = false;
                return map->aliases[i].full_modules[0];
            }

            /* Multiple modules use this alias - try type-based resolution */
            if (referenced_type && dep_cache) {
                const char *resolved_module = NULL;
                int matches = 0;

                /* Check which modules export the referenced type */
                for (int j = 0; j < module_count; j++) {
                    if (module_exports_type(dep_cache, map->aliases[i].full_modules[j], referenced_type)) {
                        resolved_module = map->aliases[i].full_modules[j];
                        matches++;
                    }
                }

                /* If exactly one module exports it, we've resolved the ambiguity! */
                if (matches == 1) {
                    if (is_ambiguous) *is_ambiguous = false;
                    return resolved_module;
                }
                /* If multiple modules export it or none export it, fall through to error handling */
            }

            /* Still ambiguous - report error with first two modules for backward compatibility */
            if (is_ambiguous) *is_ambiguous = true;
            if (ambiguous_module1 && module_count > 0) *ambiguous_module1 = map->aliases[i].full_modules[0];
            if (ambiguous_module2 && module_count > 1) *ambiguous_module2 = map->aliases[i].full_modules[1];
            return NULL;  /* Ambiguous - cannot resolve */
        }
    }
    if (is_ambiguous) *is_ambiguous = false;
    return NULL;
}

void free_module_alias_map(ModuleAliasMap *map) {
    for (int i = 0; i < map->aliases_count; i++) {
        arena_free(map->aliases[i].alias);
        for (int j = 0; j < map->aliases[i].full_modules_count; j++) {
            arena_free(map->aliases[i].full_modules[j]);
        }
        arena_free(map->aliases[i].full_modules);
    }
    arena_free(map->aliases);
}

/* ============================================================================
 * TypeAliasMap functions
 * ============================================================================ */

void init_type_alias_map(TypeAliasMap *map) {
    map->aliases = arena_malloc(16 * sizeof(TypeAliasExpansion));
    map->aliases_count = 0;
    map->aliases_capacity = 16;
}

void add_type_alias(TypeAliasMap *map, const char *type_name, char **type_vars, int type_vars_count, const char *expansion) {
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

const TypeAliasExpansion *lookup_type_alias(TypeAliasMap *map, const char *type_name) {
    for (int i = 0; i < map->aliases_count; i++) {
        if (strcmp(map->aliases[i].type_name, type_name) == 0) {
            return &map->aliases[i];
        }
    }
    return NULL;
}

void free_type_alias_map(TypeAliasMap *map) {
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

/* ============================================================================
 * ExportList functions
 * ============================================================================ */

void free_export_list(ExportList *exports) {
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

bool is_exported_value(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_values_count; i++) {
        if (strcmp(exports->exposed_values[i], name) == 0) return true;
    }
    return false;
}

bool is_exported_type(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_types_count; i++) {
        if (strcmp(exports->exposed_types[i], name) == 0) return true;
    }
    return false;
}

bool is_type_exposed_with_constructors(const char *name, ExportList *exports) {
    if (exports->expose_all) return true;
    for (int i = 0; i < exports->exposed_types_with_constructors_count; i++) {
        if (strcmp(exports->exposed_types_with_constructors[i], name) == 0) return true;
    }
    return false;
}
