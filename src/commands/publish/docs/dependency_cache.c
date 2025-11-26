#include "dependency_cache.h"
#include "../../../alloc.h"
#include "../../../elm_json.h"
#include "../../../cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tree_sitter/api.h>

/* External tree-sitter language function */
extern TSLanguage *tree_sitter_elm(void);

/* Helper to read file contents */
static char *read_file_contents(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = arena_malloc(file_size + 1);
    if (!content) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(content, 1, file_size, file);
    content[read_size] = '\0';
    fclose(file);

    return content;
}

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

/* Build a module file path from module name (e.g., "Json.Decode" -> "Json/Decode.elm") */
static char *module_name_to_file_path(const char *module_name) {
    size_t len = strlen(module_name);
    char *path = arena_malloc(len + 5);  /* +4 for ".elm" + 1 for null */
    strcpy(path, module_name);

    /* Replace dots with slashes */
    for (char *p = path; *p; p++) {
        if (*p == '.') {
            *p = '/';
        }
    }

    strcat(path, ".elm");
    return path;
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

        /* Extract the minimum version from constraint (e.g., "1.0.0 <= v < 2.0.0" -> "1.0.0")
         * The version field may contain a constraint or just a version number.
         * We parse it to extract the lower bound version. */
        char min_version[64] = {0};
        const char *v = pkg->version;

        /* Skip leading whitespace */
        while (*v && (*v == ' ' || *v == '\t')) v++;

        /* Extract version number (digits and dots) */
        int pos = 0;
        while (*v && pos < (int)sizeof(min_version) - 1) {
            if ((*v >= '0' && *v <= '9') || *v == '.') {
                min_version[pos++] = *v++;
            } else {
                break;  /* Stop at first non-version character */
            }
        }
        min_version[pos] = '\0';

        /* If we couldn't extract a version, skip this dependency */
        if (min_version[0] == '\0') {
            continue;
        }

        /* Build path to package in ELM_HOME */
        char package_dir[2048];
        snprintf(package_dir, sizeof(package_dir), "%s/packages/%s/%s/%s/src",
                 cache->elm_home, pkg->author, pkg->name, min_version);

        /* Convert module name to file path */
        rel_path = module_name_to_file_path(module_name);
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", package_dir, rel_path);
        arena_free(rel_path);

        /* Check if file exists */
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            elm_json_free(elm_json);
            return arena_strdup(full_path);
        }
    }

    elm_json_free(elm_json);
    return NULL;
}

/* Parse a module file to extract its exported types */
static CachedModuleExports *parse_module_exports(const char *module_path, const char *module_name) {
    /* Read file content */
    char *source_code = read_file_contents(module_path);
    if (!source_code) {
        /* Create a failed parse entry */
        CachedModuleExports *exports = arena_malloc(sizeof(CachedModuleExports));
        exports->module_name = arena_strdup(module_name);
        exports->exported_types = NULL;
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
    exports->exported_types_count = 0;
    exports->parsed = true;

    /* Allocate array for exported types */
    int capacity = 16;
    exports->exported_types = arena_malloc(capacity * sizeof(char*));
    
    bool expose_all = false;

    /* Find module_declaration node */
    uint32_t child_count = ts_node_child_count(root_node);
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
                                    }

                                    exports->exported_types[exports->exported_types_count++] = type_name;
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
    
    /* If module exposes all, scan the file for type definitions */
    if (expose_all) {
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(root_node, i);
            const char *type = ts_node_type(child);
            
            if (strcmp(type, "type_declaration") == 0 || strcmp(type, "type_alias_declaration") == 0) {
                /* Find the type name (upper_case_identifier) */
                uint32_t type_child_count = ts_node_child_count(child);
                for (uint32_t j = 0; j < type_child_count; j++) {
                    TSNode type_child = ts_node_child(child, j);
                    if (strcmp(ts_node_type(type_child), "upper_case_identifier") == 0) {
                        char *type_name = get_node_text(type_child, source_code);
                        
                        /* Expand array if needed */
                        if (exports->exported_types_count >= capacity) {
                            capacity *= 2;
                            exports->exported_types = arena_realloc(exports->exported_types, capacity * sizeof(char*));
                        }
                        
                        exports->exported_types[exports->exported_types_count++] = type_name;
                        break;
                    }
                }
            }
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
    cache->modules = arena_malloc(32 * sizeof(CachedModuleExports));
    cache->modules_count = 0;
    cache->modules_capacity = 32;

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
    if (!module_path) {
        /* Module not found - add a failed entry to cache */
        if (cache->modules_count >= cache->modules_capacity) {
            cache->modules_capacity *= 2;
            cache->modules = arena_realloc(cache->modules, cache->modules_capacity * sizeof(CachedModuleExports));
        }

        cache->modules[cache->modules_count].module_name = arena_strdup(module_name);
        cache->modules[cache->modules_count].exported_types = NULL;
        cache->modules[cache->modules_count].exported_types_count = 0;
        cache->modules[cache->modules_count].parsed = false;
        cache->modules_count++;

        return &cache->modules[cache->modules_count - 1];
    }

    /* Parse the module */
    CachedModuleExports *exports = parse_module_exports(module_path, module_name);
    arena_free(module_path);

    /* Add to cache */
    if (cache->modules_count >= cache->modules_capacity) {
        cache->modules_capacity *= 2;
        cache->modules = arena_realloc(cache->modules, cache->modules_capacity * sizeof(CachedModuleExports));
    }

    cache->modules[cache->modules_count++] = *exports;
    arena_free(exports);  /* We copied the struct, so free the temp allocation */

    return &cache->modules[cache->modules_count - 1];
}
