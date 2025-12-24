#include "dependency_cache.h"
#include "path_util.h"
#include "../../../alloc.h"
#include "../../../constants.h"
#include "../../../elm_json.h"
#include "../../../cache.h"
#include "../../../fileutil.h"
#include "../../../vendor/cJSON.h"
#include "../../../registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "tree_sitter/api.h"

/* External tree-sitter language function */
extern TSLanguage *tree_sitter_elm(void);

/* Helper to get node text */
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

/* Resolve the highest version matching a constraint using registry.dat
 * Returns an allocated string with the version, or NULL if not found */
static char *resolve_version_from_registry(const char *elm_home, const char *author,
                                            const char *name, const char *constraint) {
    /* Build path to registry.dat */
    char registry_path[2048];
    snprintf(registry_path, sizeof(registry_path), "%s/packages/registry.dat", elm_home);

    /* Check if registry.dat exists */
    struct stat st;
    if (stat(registry_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;  /* registry.dat doesn't exist, caller should fall back to scanning */
    }

    /* Load registry */
    Registry *registry = registry_load_from_dat(registry_path, NULL);
    if (!registry) {
        return NULL;
    }

    /* Resolve constraint to highest matching version */
    Version resolved_version;
    bool found = registry_resolve_constraint(registry, author, name, constraint, &resolved_version);

    registry_free(registry);

    if (!found) {
        return NULL;
    }

    /* Convert version to string */
    char *version_str = version_to_string(&resolved_version);
    return version_str;
}

/* Scan filesystem to find highest version matching a constraint
 * Fallback when registry.dat is not available */
static char *resolve_version_from_filesystem(const char *elm_home, const char *author,
                                              const char *name, const char *constraint) {
    /* Parse the constraint to get bounds */
    VersionRange range;
    if (!version_parse_constraint(constraint, &range)) {
        return NULL;  /* version_parse_constraint handles exact versions too */
    }

    Version lower_bound = range.lower.v;
    Version upper_bound = range.upper.v;

    /* Scan package directory for available versions */
    char package_dir[2048];
    snprintf(package_dir, sizeof(package_dir), "%s/packages/%s/%s", elm_home, author, name);

    DIR *dir = opendir(package_dir);
    if (!dir) {
        return NULL;
    }

    Version highest_version = {0, 0, 0};
    bool found_any = false;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Try to parse as version */
        Version v;
        if (version_parse_safe(entry->d_name, &v)) {

            /* Check if within constraint bounds */
            if (registry_version_compare(&v, &lower_bound) >= 0 &&
                registry_version_compare(&v, &upper_bound) < 0) {
                /* This version matches */
                if (!found_any || registry_version_compare(&v, &highest_version) > 0) {
                    highest_version = v;
                    found_any = true;
                }
            }
        }
    }

    closedir(dir);

    if (!found_any) {
        return NULL;
    }

    return version_to_string(&highest_version);
}

/* Find a module file in the package's dependencies */
static char *find_module_in_dependencies(struct DependencyCache *cache, const char *module_name) {
    /* First, check if the module exists in the current package's src/ directory */
    char *rel_path = module_name_to_file_path(module_name);
    char local_path[2048];
    snprintf(local_path, sizeof(local_path), "%s/src/%s", cache->package_path, rel_path);

    struct stat st;
    if (stat(local_path, &st) == 0 && S_ISREG(st.st_mode)) {
        arena_free(rel_path);
        return arena_strdup(local_path);
    }
    arena_free(rel_path);

    /* Not found locally, search in dependencies */
    /* Read the package's elm.json to get dependencies */
    char elm_json_path[1024];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", cache->package_path);

    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        return NULL;
    }

    /* Get dependencies based on project type */
    PackageMap *deps = NULL;
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        deps = elm_json->dependencies_direct;
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        deps = elm_json->package_dependencies;
    }

    if (!deps) {
        elm_json_free(elm_json);
        return NULL;
    }

    /* Try each dependency package */
    for (int i = 0; i < deps->count; i++) {
        Package *pkg = &deps->packages[i];

        /* Resolve to the HIGHEST version matching the constraint */
        char *resolved_version = resolve_version_from_registry(cache->elm_home, pkg->author,
                                                                 pkg->name, pkg->version);

        /* Fall back to filesystem scanning if registry lookup failed */
        if (!resolved_version) {
            resolved_version = resolve_version_from_filesystem(cache->elm_home, pkg->author,
                                                                pkg->name, pkg->version);
        }

        if (!resolved_version) {
            continue;  /* Couldn't resolve version */
        }

        /* Build path to package in ELM_HOME */
        char package_dir[2048];
        snprintf(package_dir, sizeof(package_dir), "%s/packages/%s/%s/%s/src",
                 cache->elm_home, pkg->author, pkg->name, resolved_version);

        /* Convert module name to file path */
        rel_path = module_name_to_file_path(module_name);
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", package_dir, rel_path);
        arena_free(rel_path);

        /* Check if file exists */
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            arena_free(resolved_version);
            elm_json_free(elm_json);
            return arena_strdup(full_path);
        }

        arena_free(resolved_version);
    }

    elm_json_free(elm_json);
    return NULL;
}

/* Parse a docs.json file to extract exported types from a package module */
static CachedModuleExports *parse_module_exports_from_docs_json(const char *docs_json_path, const char *module_name) {
    /* Read docs.json content */
    char *json_content = file_read_contents_bounded(docs_json_path, MAX_DOCS_JSON_FILE_BYTES, NULL);
    if (!json_content) {
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Parse JSON */
    cJSON *json = cJSON_Parse(json_content);
    arena_free(json_content);

    if (!json) {
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* docs.json is an array of module objects */
    if (!cJSON_IsArray(json)) {
        cJSON_Delete(json);
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Find the module with matching name */
    cJSON *target_module = NULL;
    cJSON *module_item = NULL;
    cJSON_ArrayForEach(module_item, json) {
        cJSON *name_item = cJSON_GetObjectItem(module_item, "name");
        if (name_item && cJSON_IsString(name_item)) {
            if (strcmp(name_item->valuestring, module_name) == 0) {
                target_module = module_item;
                break;
            }
        }
    }

    if (!target_module) {
        cJSON_Delete(json);
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Initialize exports */
    CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
    exports->module_name = arena_strdup(module_name);
    exports->parsed = true;

    int capacity = INITIAL_SMALL_CAPACITY;
    exports->exported_types = arena_malloc(capacity * sizeof(char*));
    exports->exported_types_arity = arena_malloc(capacity * sizeof(int));
    exports->exported_types_count = 0;

    /* Extract type names from "unions" array */
    cJSON *unions = cJSON_GetObjectItem(target_module, "unions");
    if (unions && cJSON_IsArray(unions)) {
        cJSON *union_item = NULL;
        cJSON_ArrayForEach(union_item, unions) {
            cJSON *name_item = cJSON_GetObjectItem(union_item, "name");
            cJSON *args_item = cJSON_GetObjectItem(union_item, "args");
            if (name_item && cJSON_IsString(name_item)) {
                if (exports->exported_types_count >= capacity) {
                    capacity *= 2;
                    exports->exported_types = arena_realloc(exports->exported_types, capacity * sizeof(char*));
                    exports->exported_types_arity = arena_realloc(exports->exported_types_arity, capacity * sizeof(int));
                }
                exports->exported_types[exports->exported_types_count] = arena_strdup(name_item->valuestring);
                /* Count type parameters from "args" array */
                int arity = 0;
                if (args_item && cJSON_IsArray(args_item)) {
                    arity = cJSON_GetArraySize(args_item);
                }
                exports->exported_types_arity[exports->exported_types_count] = arity;
                exports->exported_types_count++;
            }
        }
    }

    /* Extract type names from "aliases" array */
    cJSON *aliases = cJSON_GetObjectItem(target_module, "aliases");
    if (aliases && cJSON_IsArray(aliases)) {
        cJSON *alias_item = NULL;
        cJSON_ArrayForEach(alias_item, aliases) {
            cJSON *name_item = cJSON_GetObjectItem(alias_item, "name");
            cJSON *args_item = cJSON_GetObjectItem(alias_item, "args");
            if (name_item && cJSON_IsString(name_item)) {
                if (exports->exported_types_count >= capacity) {
                    capacity *= 2;
                    exports->exported_types = arena_realloc(exports->exported_types, capacity * sizeof(char*));
                    exports->exported_types_arity = arena_realloc(exports->exported_types_arity, capacity * sizeof(int));
                }
                exports->exported_types[exports->exported_types_count] = arena_strdup(name_item->valuestring);
                /* Count type parameters from "args" array */
                int arity = 0;
                if (args_item && cJSON_IsArray(args_item)) {
                    arity = cJSON_GetArraySize(args_item);
                }
                exports->exported_types_arity[exports->exported_types_count] = arity;
                exports->exported_types_count++;
            }
        }
    }

    cJSON_Delete(json);
    return exports;
}

/* Parse a module file to extract its exported types */
static CachedModuleExports *parse_module_exports(const char *module_path, const char *module_name) {
    /* Read file content */
    char *source_code = file_read_contents_bounded(module_path, MAX_ELM_SOURCE_FILE_BYTES, NULL);
    if (!source_code) {
        /* Create a failed parse entry */
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Create parser */
    TSParser *parser = ts_parser_new();
    if (!parser) {
        arena_free(source_code);
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Set the Elm language */
    if (!ts_parser_set_language(parser, tree_sitter_elm())) {
        ts_parser_delete(parser);
        arena_free(source_code);
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Parse the source code */
    TSTree *tree = ts_parser_parse_string(parser, NULL, source_code, strlen(source_code));
    if (!tree) {
        ts_parser_delete(parser);
        arena_free(source_code);
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
        return exports;
    }

    /* Get the root node */
    TSNode root_node = ts_tree_root_node(tree);

    /* Initialize exports */
    CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
    exports->module_name = arena_strdup(module_name);
    exports->exported_types = NULL;
    exports->exported_types_arity = NULL;
    exports->exported_types_count = 0;
    exports->parsed = true;

    /* Allocate array for exported types */
    int capacity = INITIAL_SMALL_CAPACITY;
    exports->exported_types = arena_malloc(capacity * sizeof(char*));
    exports->exported_types_arity = arena_malloc(capacity * sizeof(int));

    bool expose_all = false;
    uint32_t child_count = ts_node_child_count(root_node);

    /* First pass: build a map of all type declarations to their arities */
    typedef struct {
        char *name;
        int arity;
    } TypeInfo;
    TypeInfo *type_infos = arena_malloc(INITIAL_MEDIUM_CAPACITY * sizeof(TypeInfo));
    int type_infos_count = 0;
    int type_infos_capacity = INITIAL_MEDIUM_CAPACITY;

    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        const char *node_type = ts_node_type(child);

        if (strcmp(node_type, "type_declaration") == 0 || strcmp(node_type, "type_alias_declaration") == 0) {
            uint32_t decl_child_count = ts_node_child_count(child);
            char *type_name = NULL;
            int arity = 0;

            for (uint32_t j = 0; j < decl_child_count; j++) {
                TSNode decl_child = ts_node_child(child, j);
                const char *child_type = ts_node_type(decl_child);

                if (strcmp(child_type, "upper_case_identifier") == 0 && !type_name) {
                    type_name = get_node_text(decl_child, source_code);
                } else if (strcmp(child_type, "lower_type_name") == 0) {
                    arity++;
                }
            }

            if (type_name) {
                if (type_infos_count >= type_infos_capacity) {
                    type_infos_capacity *= 2;
                    type_infos = arena_realloc(type_infos, type_infos_capacity * sizeof(TypeInfo));
                }
                type_infos[type_infos_count].name = type_name;
                type_infos[type_infos_count].arity = arity;
                type_infos_count++;
            }
        }
    }

    /* Find module_declaration node */
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(root_node, i);
        const char *type = ts_node_type(child);

        if (strcmp(type, "module_declaration") == 0) {
            /* Find the exposing_list */
            uint32_t mod_child_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < mod_child_count; j++) {
                TSNode mod_child = ts_node_child(child, j);
                const char *mod_type = ts_node_type(mod_child);

                if (strcmp(mod_type, "exposing_list") == 0) {
                    /* Parse exposing list */
                    uint32_t exp_child_count = ts_node_child_count(mod_child);

                    for (uint32_t k = 0; k < exp_child_count; k++) {
                        TSNode exp_child = ts_node_child(mod_child, k);
                        const char *exp_type = ts_node_type(exp_child);

                        if (strcmp(exp_type, "double_dot") == 0) {
                            /* Module exposes everything - need to scan for all type definitions */
                            expose_all = true;
                        } else if (strcmp(exp_type, "exposed_type") == 0) {
                            /* Extract type name */
                            uint32_t type_child_count = ts_node_child_count(exp_child);
                            for (uint32_t m = 0; m < type_child_count; m++) {
                                TSNode type_child = ts_node_child(exp_child, m);
                                if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                                    char *type_name = get_node_text(type_child, source_code);

                                    /* Expand array if needed */
                                    if (exports->exported_types_count >= capacity) {
                                        capacity *= 2;
                                        exports->exported_types = arena_realloc(exports->exported_types, capacity * sizeof(char*));
                                        exports->exported_types_arity = arena_realloc(exports->exported_types_arity, capacity * sizeof(int));
                                    }

                                    /* Look up arity from our map */
                                    int arity = -1;
                                    for (int k = 0; k < type_infos_count; k++) {
                                        if (strcmp(type_infos[k].name, type_name) == 0) {
                                            arity = type_infos[k].arity;
                                            break;
                                        }
                                    }

                                    exports->exported_types[exports->exported_types_count] = type_name;
                                    exports->exported_types_arity[exports->exported_types_count] = arity;
                                    exports->exported_types_count++;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
    }
    
    /* If module exposes all, add all types from our map */
    if (expose_all) {
        for (int i = 0; i < type_infos_count; i++) {
            /* Expand array if needed */
            if (exports->exported_types_count >= capacity) {
                capacity *= 2;
                exports->exported_types = arena_realloc(exports->exported_types, capacity * sizeof(char*));
                exports->exported_types_arity = arena_realloc(exports->exported_types_arity, capacity * sizeof(int));
            }

            exports->exported_types[exports->exported_types_count] = type_infos[i].name;
            exports->exported_types_arity[exports->exported_types_count] = type_infos[i].arity;
            exports->exported_types_count++;
        }
    }

    /* Cleanup */
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    arena_free(source_code);

    return exports;
}

/* Create a new dependency cache */
struct DependencyCache* dependency_cache_create(const char *elm_home, const char *package_path) {
    struct DependencyCache *cache = arena_malloc(sizeof(struct DependencyCache));
    if (!cache) return NULL;

    /* Check if package_path is inside a package repository structure like:
     * /path/to/repository/0.19.1/packages/author/name/version
     * If so, use the repository root (up to and including the version dir) as elm_home */
    const char *effective_elm_home = elm_home;
    const char *packages_marker = strstr(package_path, "/packages/");
    if (packages_marker) {
        /* Extract everything before /packages/ (e.g., "/path/to/repository/0.19.1") */
        size_t len = packages_marker - package_path;
        char *repo_root = arena_malloc(len + 1);
        memcpy(repo_root, package_path, len);
        repo_root[len] = '\0';
        effective_elm_home = repo_root;
    }

    cache->elm_home = arena_strdup(effective_elm_home);
    cache->package_path = arena_strdup(package_path);
    cache->modules = arena_malloc(INITIAL_CONNECTION_CAPACITY * sizeof(CachedModuleExports));
    cache->modules_count = 0;
    cache->modules_capacity = INITIAL_CONNECTION_CAPACITY;

    return cache;
}

/* Free the dependency cache */
void dependency_cache_free(struct DependencyCache *cache) {
    if (!cache) return;

    arena_free(cache->elm_home);
    arena_free(cache->package_path);

    for (int i = 0; i < cache->modules_count; i++) {
        arena_free(cache->modules[i].module_name);
        for (int j = 0; j < cache->modules[i].exported_types_count; j++) {
            arena_free(cache->modules[i].exported_types[j]);
        }
        arena_free(cache->modules[i].exported_types);
        arena_free(cache->modules[i].exported_types_arity);
    }
    arena_free(cache->modules);
    arena_free(cache);
}

/* Find a module in the cache */
CachedModuleExports* dependency_cache_find(struct DependencyCache *cache, const char *module_name) {
    if (!cache || !module_name) return NULL;

    for (int i = 0; i < cache->modules_count; i++) {
        if (strcmp(cache->modules[i].module_name, module_name) == 0) {
            return &cache->modules[i];
        }
    }

    return NULL;
}

/* Get or parse module exports (lazy loading) */
CachedModuleExports* dependency_cache_get_exports(struct DependencyCache *cache, const char *module_name) {
    if (!cache || !module_name) return NULL;

    /* Check if already cached */
    CachedModuleExports *cached = dependency_cache_find(cache, module_name);
    if (cached) {
        return cached;
    }

    /* Find the module file */
    char *module_path = find_module_in_dependencies(cache, module_name);
    CachedModuleExports *exports = NULL;

    if (module_path) {
        /* Parse the module from source */
        exports = parse_module_exports(module_path, module_name);
        arena_free(module_path);
    }

    /* If source parsing failed or no source file exists, try docs.json */
    if (!exports || !exports->parsed) {
        /* Try to find docs.json in dependency packages */
        ElmJson *elm_json = NULL;
        char elm_json_path[1024];
        snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", cache->package_path);
        elm_json = elm_json_read(elm_json_path);

        if (elm_json) {
            PackageMap *deps = NULL;
            if (elm_json->type == ELM_PROJECT_APPLICATION) {
                deps = elm_json->dependencies_direct;
            } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
                deps = elm_json->package_dependencies;
            }

            if (deps) {
                for (int i = 0; i < deps->count; i++) {
                    Package *pkg = &deps->packages[i];

                    /* Resolve to the HIGHEST version matching the constraint */
                    char *resolved_version = resolve_version_from_registry(cache->elm_home, pkg->author,
                                                                             pkg->name, pkg->version);

                    /* Fall back to filesystem scanning if registry lookup failed */
                    if (!resolved_version) {
                        resolved_version = resolve_version_from_filesystem(cache->elm_home, pkg->author,
                                                                            pkg->name, pkg->version);
                    }

                    if (!resolved_version) {
                        continue;  /* Couldn't resolve version */
                    }

                    /* Try to read docs.json from package directory */
                    char docs_json_path[2048];
                    snprintf(docs_json_path, sizeof(docs_json_path), "%s/packages/%s/%s/%s/docs.json",
                             cache->elm_home, pkg->author, pkg->name, resolved_version);

                    struct stat st;
                    if (stat(docs_json_path, &st) == 0 && S_ISREG(st.st_mode)) {
                        /* Try to parse this docs.json */
                        CachedModuleExports *docs_exports = parse_module_exports_from_docs_json(docs_json_path, module_name);
                        if (docs_exports && docs_exports->parsed) {
                            if (exports) arena_free(exports);
                            exports = docs_exports;
                            arena_free(resolved_version);
                            break;
                        }
                        if (docs_exports) arena_free(docs_exports);
                    }

                    arena_free(resolved_version);
                }
            }
            elm_json_free(elm_json);
        }
    }

    /* If still no exports, create a failed entry */
    if (!exports) {
        exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
        exports->exported_types_arity = NULL;
        exports->exported_types_count = 0;
        exports->parsed = false;
    }

    /* Add to cache */
    if (cache->modules_count >= cache->modules_capacity) {
        cache->modules_capacity *= 2;
        cache->modules = arena_realloc(cache->modules, cache->modules_capacity * sizeof(CachedModuleExports));
    }

    cache->modules[cache->modules_count++] = *exports;
    arena_free(exports);  /* We copied the struct, so free the temp allocation */

    return &cache->modules[cache->modules_count - 1];
}
