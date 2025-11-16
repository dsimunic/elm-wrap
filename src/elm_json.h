#ifndef ELM_JSON_H
#define ELM_JSON_H

#include <stdbool.h>

/* Forward declarations */
typedef struct ElmJson ElmJson;
typedef struct Package Package;
typedef struct PackageMap PackageMap;

/* Package representation */
struct Package {
    char *author;
    char *name;
    char *version;  // e.g., "1.0.0"
};

/* Package map (author/package -> version) */
struct PackageMap {
    Package *packages;
    int count;
    int capacity;
};

/* Elm project types */
typedef enum {
    ELM_PROJECT_APPLICATION,
    ELM_PROJECT_PACKAGE
} ElmProjectType;

/* Elm.json structure */
struct ElmJson {
    ElmProjectType type;
    char *elm_version;  // e.g., "0.19.1"
    
    /* Application-specific fields */
    PackageMap *dependencies_direct;
    PackageMap *dependencies_indirect;
    PackageMap *dependencies_test_direct;
    PackageMap *dependencies_test_indirect;
    
    /* Package-specific fields */
    char *package_name;     // e.g., "author/package"
    char *package_version;  // e.g., "1.0.0"
    PackageMap *package_dependencies;
    PackageMap *package_test_dependencies;
};

/* Package map operations */
PackageMap* package_map_create(void);
void package_map_free(PackageMap *map);
bool package_map_add(PackageMap *map, const char *author, const char *name, const char *version);
Package* package_map_find(PackageMap *map, const char *author, const char *name);
bool package_map_remove(PackageMap *map, const char *author, const char *name);
void package_map_print(PackageMap *map);

/* Elm.json operations */
ElmJson* elm_json_read(const char *filepath);
bool elm_json_write(ElmJson *elm_json, const char *filepath);
void elm_json_free(ElmJson *elm_json);

/* Dependency promotion */
typedef enum {
    PROMOTION_NONE,
    PROMOTION_INDIRECT_TO_DIRECT,
    PROMOTION_TEST_TO_DIRECT,
    PROMOTION_TEST_INDIRECT_TO_TEST_DIRECT
} PromotionType;

PromotionType elm_json_find_package(ElmJson *elm_json, const char *author, const char *name);
bool elm_json_promote_package(ElmJson *elm_json, const char *author, const char *name);

/* Utility functions */
void package_free(Package *pkg);
Package* package_create(const char *author, const char *name, const char *version);

#endif /* ELM_JSON_H */
