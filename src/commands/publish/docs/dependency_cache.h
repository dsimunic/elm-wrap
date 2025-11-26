#ifndef DEPENDENCY_CACHE_H
#define DEPENDENCY_CACHE_H

#include <stdbool.h>

/* Forward declaration - typedef is provided by including file */
typedef struct CachedModuleExports CachedModuleExports;

/* Note: DependencyCache is forward-declared in elm_docs.h */
struct DependencyCache;

/* Cached exports for a single module */
struct CachedModuleExports {
    char *module_name;           /* e.g., "Json.Decode" */
    char **exported_types;       /* e.g., ["Decoder", "Value", "Error"] */
    int exported_types_count;
    bool parsed;                 /* false if lookup failed or not yet parsed */
};

/* Main dependency cache structure definition */
struct DependencyCache {
    CachedModuleExports *modules;
    int modules_count;
    int modules_capacity;

    char *elm_home;              /* Path to ELM_HOME directory */
    char *package_path;          /* Path to the package being documented */
};

/* Cache lifecycle */
struct DependencyCache* dependency_cache_create(const char *elm_home, const char *package_path);
void dependency_cache_free(struct DependencyCache *cache);

/* Get or parse module exports (lazy loading) */
CachedModuleExports* dependency_cache_get_exports(struct DependencyCache *cache, const char *module_name);

/* Helper to check if a module exists in cache */
CachedModuleExports* dependency_cache_find(struct DependencyCache *cache, const char *module_name);

#endif /* DEPENDENCY_CACHE_H */
