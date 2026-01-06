#include "elm_json.h"
#include "alloc.h"
#include "constants.h"
#include "fileutil.h"
#include "vendor/cJSON.h"
#include "shared/log.h"
#include "commands/package/package_common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/* Package operations */
Package* package_create(const char *author, const char *name, const char *version) {
    Package *pkg = arena_malloc(sizeof(Package));
    if (!pkg) return NULL;
    
    pkg->author = arena_strdup(author);
    pkg->name = arena_strdup(name);
    pkg->version = arena_strdup(version);
    
    return pkg;
}

void package_free(Package *pkg) {
    if (!pkg) return;
    arena_free(pkg->author);
    arena_free(pkg->name);
    arena_free(pkg->version);
    arena_free(pkg);
}

/* Package map operations */
PackageMap* package_map_create(void) {
    PackageMap *map = arena_malloc(sizeof(PackageMap));
    if (!map) return NULL;

    map->capacity = INITIAL_SMALL_CAPACITY;
    map->count = 0;
    map->packages = arena_malloc(sizeof(Package) * map->capacity);
    
    if (!map->packages) {
        arena_free(map);
        return NULL;
    }
    
    return map;
}

void package_map_free(PackageMap *map) {
    if (!map) return;
    
    for (int i = 0; i < map->count; i++) {
        arena_free(map->packages[i].author);
        arena_free(map->packages[i].name);
        arena_free(map->packages[i].version);
    }
    
    arena_free(map->packages);
    arena_free(map);
}

bool package_map_add(PackageMap *map, const char *author, const char *name, const char *version) {
    if (!map || !author || !name || !version) return false;
    
    // Check if package already exists
    if (package_map_find(map, author, name)) {
        return false;
    }
    
    // Expand capacity if needed
    if (map->count >= map->capacity) {
        int new_capacity = map->capacity * 2;
        Package *new_packages = arena_realloc(map->packages, sizeof(Package) * (size_t)new_capacity);
        if (!new_packages) return false;
        map->packages = new_packages;
        map->capacity = new_capacity;
    }

    char *author_copy = arena_strdup(author);
    char *name_copy = arena_strdup(name);
    char *version_copy = arena_strdup(version);
    if (!author_copy || !name_copy || !version_copy) {
        arena_free(author_copy);
        arena_free(name_copy);
        arena_free(version_copy);
        return false;
    }

    // Add package
    map->packages[map->count].author = author_copy;
    map->packages[map->count].name = name_copy;
    map->packages[map->count].version = version_copy;
    map->count++;
    return true;
}

Package* package_map_find(PackageMap *map, const char *author, const char *name) {
    if (!map || !author || !name) return NULL;
    
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->packages[i].author, author) == 0 &&
            strcmp(map->packages[i].name, name) == 0) {
            return &map->packages[i];
        }
    }
    
    return NULL;
}

bool package_map_remove(PackageMap *map, const char *author, const char *name) {
    if (!map || !author || !name) return false;
    
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->packages[i].author, author) == 0 &&
            strcmp(map->packages[i].name, name) == 0) {
            
            // Free strings
            arena_free(map->packages[i].author);
            arena_free(map->packages[i].name);
            arena_free(map->packages[i].version);
            
            // Move last element to this position
            if (i < map->count - 1) {
                map->packages[i] = map->packages[map->count - 1];
            }
            
            map->count--;
            return true;
        }
    }
    
    return false;
}

void package_map_print(PackageMap *map) {
    if (!map) return;
    
    for (int i = 0; i < map->count; i++) {
        log_progress("    \"%s/%s\": \"%s\"", 
               map->packages[i].author, 
               map->packages[i].name,
               map->packages[i].version);
    }
}

/* Elm.json operations */
ElmJson* elm_json_read(const char *filepath) {
    char *data = file_read_contents_bounded(filepath, MAX_ELM_JSON_FILE_BYTES, NULL);
    if (!data) {
        // In verbose mode, show both the path and the resolved absolute path
        char abs_path[MAX_PATH_LENGTH];
        if (realpath(filepath, abs_path)) {
            log_debug("Could not read '%s' (resolved: %s)", filepath, abs_path);
        } else {
            // realpath failed, show cwd for context
            char cwd[MAX_TEMP_PATH_LENGTH];
            if (getcwd(cwd, sizeof(cwd))) {
                log_debug("Could not read '%s' (cwd: %s)", filepath, cwd);
            } else {
                log_debug("Could not read '%s'", filepath);
            }
        }
        return NULL;
    }

    // Parse JSON
    cJSON *json = cJSON_Parse(data);
    arena_free(data);
    
    if (!json) {
        log_error("Failed to parse JSON in %s", filepath);
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            log_error("Error before: %s", error_ptr);
        }
        return NULL;
    }
    
    // Create ElmJson structure
    ElmJson *elm_json = arena_malloc(sizeof(ElmJson));
    if (!elm_json) {
        cJSON_Delete(json);
        return NULL;
    }
    
    // Parse project type
    cJSON *type_item = cJSON_GetObjectItem(json, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        log_error("No 'type' field in %s", filepath);
        cJSON_Delete(json);
        arena_free(elm_json);
        return NULL;
    }
    
    if (strcmp(type_item->valuestring, "application") == 0) {
        elm_json->type = ELM_PROJECT_APPLICATION;
    } else if (strcmp(type_item->valuestring, "package") == 0) {
        elm_json->type = ELM_PROJECT_PACKAGE;
    } else {
        log_error("Invalid project type: %s", type_item->valuestring);
        arena_free(elm_json);
        cJSON_Delete(json);
        return NULL;
    }
    
    // Parse elm-version
    cJSON *elm_version_item = cJSON_GetObjectItem(json, "elm-version");
    if (elm_version_item && cJSON_IsString(elm_version_item)) {
        elm_json->elm_version = arena_strdup(elm_version_item->valuestring);
    } else {
        elm_json->elm_version = arena_strdup("0.19.1");
    }
    
    // Initialize package maps
    elm_json->dependencies_direct = package_map_create();
    elm_json->dependencies_indirect = package_map_create();
    elm_json->dependencies_test_direct = package_map_create();
    elm_json->dependencies_test_indirect = package_map_create();
    elm_json->package_name = NULL;
    elm_json->package_version = NULL;
    elm_json->package_dependencies = NULL;
    elm_json->package_test_dependencies = NULL;

    if (!elm_json->dependencies_direct ||
        !elm_json->dependencies_indirect ||
        !elm_json->dependencies_test_direct ||
        !elm_json->dependencies_test_indirect) {
        cJSON_Delete(json);
        elm_json_free(elm_json);
        return NULL;
    }
    
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        // Parse dependencies
        cJSON *deps = cJSON_GetObjectItem(json, "dependencies");
        if (deps && cJSON_IsObject(deps)) {
            // Parse direct dependencies
            cJSON *direct = cJSON_GetObjectItem(deps, "direct");
            if (direct && cJSON_IsObject(direct)) {
                size_t entries = 0;
                cJSON *package = NULL;
                cJSON_ArrayForEach(package, direct) {
                    if (++entries > MAX_ELM_JSON_DEPENDENCY_ENTRIES) {
                        log_error("Too many dependencies in %s (direct)", filepath);
                        cJSON_Delete(json);
                        elm_json_free(elm_json);
                        return NULL;
                    }
                    if (cJSON_IsString(package)) {
                        // Parse "author/name"
                        const char *full_name = package->string;
                        if (!package->valuestring ||
                            strlen(package->valuestring) >= MAX_ELM_JSON_VERSION_VALUE_LENGTH) {
                            log_error("Invalid dependency version string in %s", filepath);
                            cJSON_Delete(json);
                            elm_json_free(elm_json);
                            return NULL;
                        }
                        char *author = NULL;
                        char *name = NULL;
                        if (parse_package_name(full_name, &author, &name)) {
                            if (!package_map_add(elm_json->dependencies_direct, author, name, package->valuestring)) {
                                arena_free(author);
                                arena_free(name);
                                cJSON_Delete(json);
                                elm_json_free(elm_json);
                                return NULL;
                            }
                            arena_free(author);
                            arena_free(name);
                        }
                    }
                }
            }
            
            // Parse indirect dependencies
            cJSON *indirect = cJSON_GetObjectItem(deps, "indirect");
            if (indirect && cJSON_IsObject(indirect)) {
                size_t entries = 0;
                cJSON *package = NULL;
                cJSON_ArrayForEach(package, indirect) {
                    if (++entries > MAX_ELM_JSON_DEPENDENCY_ENTRIES) {
                        log_error("Too many dependencies in %s (indirect)", filepath);
                        cJSON_Delete(json);
                        elm_json_free(elm_json);
                        return NULL;
                    }
                    if (cJSON_IsString(package)) {
                        const char *full_name = package->string;
                        if (!package->valuestring ||
                            strlen(package->valuestring) >= MAX_ELM_JSON_VERSION_VALUE_LENGTH) {
                            log_error("Invalid dependency version string in %s", filepath);
                            cJSON_Delete(json);
                            elm_json_free(elm_json);
                            return NULL;
                        }
                        char *author = NULL;
                        char *name = NULL;
                        if (parse_package_name(full_name, &author, &name)) {
                            if (!package_map_add(elm_json->dependencies_indirect, author, name, package->valuestring)) {
                                arena_free(author);
                                arena_free(name);
                                cJSON_Delete(json);
                                elm_json_free(elm_json);
                                return NULL;
                            }
                            arena_free(author);
                            arena_free(name);
                        }
                    }
                }
            }
        }
        
        // Parse test-dependencies
        cJSON *test_deps = cJSON_GetObjectItem(json, "test-dependencies");
        if (test_deps && cJSON_IsObject(test_deps)) {
            // Parse test direct dependencies
            cJSON *test_direct = cJSON_GetObjectItem(test_deps, "direct");
            if (test_direct && cJSON_IsObject(test_direct)) {
                size_t entries = 0;
                cJSON *package = NULL;
                cJSON_ArrayForEach(package, test_direct) {
                    if (++entries > MAX_ELM_JSON_DEPENDENCY_ENTRIES) {
                        log_error("Too many test-dependencies in %s (direct)", filepath);
                        cJSON_Delete(json);
                        elm_json_free(elm_json);
                        return NULL;
                    }
                    if (cJSON_IsString(package)) {
                        const char *full_name = package->string;
                        if (!package->valuestring ||
                            strlen(package->valuestring) >= MAX_ELM_JSON_VERSION_VALUE_LENGTH) {
                            log_error("Invalid dependency version string in %s", filepath);
                            cJSON_Delete(json);
                            elm_json_free(elm_json);
                            return NULL;
                        }
                        char *author = NULL;
                        char *name = NULL;
                        if (parse_package_name(full_name, &author, &name)) {
                            if (!package_map_add(elm_json->dependencies_test_direct, author, name, package->valuestring)) {
                                arena_free(author);
                                arena_free(name);
                                cJSON_Delete(json);
                                elm_json_free(elm_json);
                                return NULL;
                            }
                            arena_free(author);
                            arena_free(name);
                        }
                    }
                }
            }
            
            // Parse test indirect dependencies
            cJSON *test_indirect = cJSON_GetObjectItem(test_deps, "indirect");
            if (test_indirect && cJSON_IsObject(test_indirect)) {
                size_t entries = 0;
                cJSON *package = NULL;
                cJSON_ArrayForEach(package, test_indirect) {
                    if (++entries > MAX_ELM_JSON_DEPENDENCY_ENTRIES) {
                        log_error("Too many test-dependencies in %s (indirect)", filepath);
                        cJSON_Delete(json);
                        elm_json_free(elm_json);
                        return NULL;
                    }
                    if (cJSON_IsString(package)) {
                        const char *full_name = package->string;
                        if (!package->valuestring ||
                            strlen(package->valuestring) >= MAX_ELM_JSON_VERSION_VALUE_LENGTH) {
                            log_error("Invalid dependency version string in %s", filepath);
                            cJSON_Delete(json);
                            elm_json_free(elm_json);
                            return NULL;
                        }
                        char *author = NULL;
                        char *name = NULL;
                        if (parse_package_name(full_name, &author, &name)) {
                            if (!package_map_add(elm_json->dependencies_test_indirect, author, name, package->valuestring)) {
                                arena_free(author);
                                arena_free(name);
                                cJSON_Delete(json);
                                elm_json_free(elm_json);
                                return NULL;
                            }
                            arena_free(author);
                            arena_free(name);
                        }
                    }
                }
            }
        }
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        // Parse package name and version
        cJSON *name_item = cJSON_GetObjectItem(json, "name");
        if (name_item && cJSON_IsString(name_item)) {
            elm_json->package_name = arena_strdup(name_item->valuestring);
        }
        
        cJSON *version_item = cJSON_GetObjectItem(json, "version");
        if (version_item && cJSON_IsString(version_item)) {
            elm_json->package_version = arena_strdup(version_item->valuestring);
        }
        
        // Initialize package dependencies
        elm_json->package_dependencies = package_map_create();
        elm_json->package_test_dependencies = package_map_create();

        if (!elm_json->package_dependencies || !elm_json->package_test_dependencies) {
            cJSON_Delete(json);
            elm_json_free(elm_json);
            return NULL;
        }
        
        // Parse dependencies (for packages, these are constraint ranges)
        cJSON *deps = cJSON_GetObjectItem(json, "dependencies");
        if (deps && cJSON_IsObject(deps)) {
            size_t entries = 0;
            cJSON *package = NULL;
            cJSON_ArrayForEach(package, deps) {
                if (++entries > MAX_ELM_JSON_DEPENDENCY_ENTRIES) {
                    log_error("Too many dependencies in %s", filepath);
                    cJSON_Delete(json);
                    elm_json_free(elm_json);
                    return NULL;
                }
                if (cJSON_IsString(package)) {
                    const char *full_name = package->string;
                    if (!package->valuestring ||
                        strlen(package->valuestring) >= MAX_ELM_JSON_VERSION_VALUE_LENGTH) {
                        log_error("Invalid dependency constraint string in %s", filepath);
                        cJSON_Delete(json);
                        elm_json_free(elm_json);
                        return NULL;
                    }
                    char *author = NULL;
                    char *name = NULL;
                    if (parse_package_name(full_name, &author, &name)) {
                        if (!package_map_add(elm_json->package_dependencies, author, name, package->valuestring)) {
                            arena_free(author);
                            arena_free(name);
                            cJSON_Delete(json);
                            elm_json_free(elm_json);
                            return NULL;
                        }
                        arena_free(author);
                        arena_free(name);
                    }
                }
            }
        }
        
        // Parse test-dependencies
        cJSON *test_deps = cJSON_GetObjectItem(json, "test-dependencies");
        if (test_deps && cJSON_IsObject(test_deps)) {
            size_t entries = 0;
            cJSON *package = NULL;
            cJSON_ArrayForEach(package, test_deps) {
                if (++entries > MAX_ELM_JSON_DEPENDENCY_ENTRIES) {
                    log_error("Too many test-dependencies in %s", filepath);
                    cJSON_Delete(json);
                    elm_json_free(elm_json);
                    return NULL;
                }
                if (cJSON_IsString(package)) {
                    const char *full_name = package->string;
                    if (!package->valuestring ||
                        strlen(package->valuestring) >= MAX_ELM_JSON_VERSION_VALUE_LENGTH) {
                        log_error("Invalid dependency constraint string in %s", filepath);
                        cJSON_Delete(json);
                        elm_json_free(elm_json);
                        return NULL;
                    }
                    char *author = NULL;
                    char *name = NULL;
                    if (parse_package_name(full_name, &author, &name)) {
                        if (!package_map_add(elm_json->package_test_dependencies, author, name, package->valuestring)) {
                            arena_free(author);
                            arena_free(name);
                            cJSON_Delete(json);
                            elm_json_free(elm_json);
                            return NULL;
                        }
                        arena_free(author);
                        arena_free(name);
                    }
                }
            }
        }
    }
    
    cJSON_Delete(json);
    return elm_json;
}

/* Comparison function for sorting packages by author/name (case-sensitive) */
static int package_compare(const void *a, const void *b) {
    const Package *pkg_a = (const Package *)a;
    const Package *pkg_b = (const Package *)b;
    
    // First compare by author
    int author_cmp = strcmp(pkg_a->author, pkg_b->author);
    if (author_cmp != 0) {
        return author_cmp;
    }
    
    // If authors are the same, compare by name
    return strcmp(pkg_a->name, pkg_b->name);
}

/* Sort packages in a PackageMap alphabetically by author/name */
static void package_map_sort(PackageMap *map) {
    if (!map || map->count <= 1) return;
    qsort(map->packages, map->count, sizeof(Package), package_compare);
}

/* Custom JSON formatter that matches Elm's formatting:
 * - 4-space indentation
 * - Sorted object keys
 * - Proper array formatting with newlines
 * - Trailing newline
 */
static bool write_elm_json_formatted_to_file(cJSON *json, FILE *file) {
    if (!json || !file) {
        return false;
    }
    
    // Helper function to write indentation
    #define INDENT(level) for (int _i = 0; _i < (level); _i++) fputs("    ", file)
    
    // Write opening brace
    fputs("{\n", file);
    
    // Get all object keys and count them
    cJSON *item = json->child;
    int item_count = 0;
    while (item) {
        item_count++;
        item = item->next;
    }
    
    // Write each top-level key
    item = json->child;
    int current_item = 0;
    while (item) {
        current_item++;
        INDENT(1);
        fprintf(file, "\"%s\": ", item->string);
        
        if (cJSON_IsString(item)) {
            fprintf(file, "\"%s\"", item->valuestring);
        } else if (cJSON_IsArray(item)) {
            // Format arrays with newlines
            fputs("[\n", file);
            cJSON *arr_item = item->child;
            int arr_count = 0;
            cJSON *temp = arr_item;
            while (temp) { arr_count++; temp = temp->next; }
            
            int arr_idx = 0;
            while (arr_item) {
                arr_idx++;
                INDENT(2);
                if (cJSON_IsString(arr_item)) {
                    fprintf(file, "\"%s\"", arr_item->valuestring);
                }
                if (arr_idx < arr_count) fputs(",", file);
                fputs("\n", file);
                arr_item = arr_item->next;
            }
            INDENT(1);
            fputs("]", file);
        } else if (cJSON_IsObject(item)) {
            // Format nested objects
            fputs("{\n", file);
            
            cJSON *obj_item = item->child;
            int obj_count = 0;
            cJSON *temp = obj_item;
            while (temp) { obj_count++; temp = temp->next; }
            
            int obj_idx = 0;
            while (obj_item) {
                obj_idx++;
                INDENT(2);
                fprintf(file, "\"%s\": ", obj_item->string);
                
                if (cJSON_IsString(obj_item)) {
                    fprintf(file, "\"%s\"", obj_item->valuestring);
                } else if (cJSON_IsObject(obj_item)) {
                    // Nested object (for "direct" and "indirect")
                    // Check if empty - if so, write on single line
                    if (!obj_item->child) {
                        fputs("{}", file);
                    } else {
                        fputs("{\n", file);

                        cJSON *nested_item = obj_item->child;
                        int nested_count = 0;
                        temp = nested_item;
                        while (temp) { nested_count++; temp = temp->next; }

                        int nested_idx = 0;
                        while (nested_item) {
                            nested_idx++;
                            INDENT(3);
                            fprintf(file, "\"%s\": \"%s\"", nested_item->string, nested_item->valuestring);
                            if (nested_idx < nested_count) fputs(",", file);
                            fputs("\n", file);
                            nested_item = nested_item->next;
                        }

                        INDENT(2);
                        fputs("}", file);
                    }
                }
                
                if (obj_idx < obj_count) fputs(",", file);
                fputs("\n", file);
                obj_item = obj_item->next;
            }
            
            INDENT(1);
            fputs("}", file);
        }
        
        if (current_item < item_count) fputs(",", file);
        fputs("\n", file);
        item = item->next;
    }
    
    fputs("}\n", file);

    #undef INDENT
    return true;
}

bool elm_json_write_formatted_atomic(struct cJSON *json, const char *filepath) {
    if (!json || !filepath) {
        return false;
    }

    size_t tmp_len = strlen(filepath) + sizeof(".tmp");
    char *tmp_path = arena_malloc(tmp_len);
    if (!tmp_path) {
        return false;
    }
    snprintf(tmp_path, tmp_len, "%s.tmp", filepath);

    FILE *file = fopen(tmp_path, "w");
    if (!file) {
        log_error("Could not open %s for writing: %s", tmp_path, strerror(errno));
        arena_free(tmp_path);
        return false;
    }

    bool ok = write_elm_json_formatted_to_file(json, file);
    if (!ok) {
        fclose(file);
        unlink(tmp_path);
        arena_free(tmp_path);
        return false;
    }

    fflush(file);
    fsync(fileno(file));
    fclose(file);

    if (rename(tmp_path, filepath) != 0) {
        log_error("Could not replace %s: %s", filepath, strerror(errno));
        unlink(tmp_path);
        arena_free(tmp_path);
        return false;
    }

    arena_free(tmp_path);
    return true;
}

bool elm_json_write(ElmJson *elm_json, const char *filepath) {
    if (!elm_json || !filepath) return false;
    
    // Sort all package maps before writing
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        package_map_sort(elm_json->dependencies_direct);
        package_map_sort(elm_json->dependencies_indirect);
        package_map_sort(elm_json->dependencies_test_direct);
        package_map_sort(elm_json->dependencies_test_indirect);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        if (elm_json->package_dependencies) {
            package_map_sort(elm_json->package_dependencies);
        }
        if (elm_json->package_test_dependencies) {
            package_map_sort(elm_json->package_test_dependencies);
        }
    }
    
    // Read the existing file to preserve all fields (bounded)
    char *file_content = file_read_contents_bounded(filepath, MAX_ELM_JSON_FILE_BYTES, NULL);
    if (!file_content) {
        log_error("Could not read %s (missing, too large, or unreadable)", filepath);
        return false;
    }
    
    // Parse the existing JSON
    cJSON *json = cJSON_Parse(file_content);
    arena_free(file_content);
    
    if (!json) {
        log_error("Failed to parse existing elm.json");
        return false;
    }
    
    // Update only the dependency fields based on project type
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        // Update dependencies object
        cJSON *deps = cJSON_GetObjectItem(json, "dependencies");
        if (deps) {
            cJSON_DeleteItemFromObject(json, "dependencies");
        }
        deps = cJSON_CreateObject();
        
        // Add direct dependencies
        cJSON *direct = cJSON_CreateObject();
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            char key[MAX_KEY_LENGTH];
            snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
            cJSON_AddStringToObject(direct, key, pkg->version);
        }
        cJSON_AddItemToObject(deps, "direct", direct);
        
        // Add indirect dependencies
        cJSON *indirect = cJSON_CreateObject();
        for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_indirect->packages[i];
            char key[MAX_KEY_LENGTH];
            snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
            cJSON_AddStringToObject(indirect, key, pkg->version);
        }
        cJSON_AddItemToObject(deps, "indirect", indirect);
        
        cJSON_AddItemToObject(json, "dependencies", deps);
        
        // Update test-dependencies object
        cJSON *test_deps = cJSON_GetObjectItem(json, "test-dependencies");
        if (test_deps) {
            cJSON_DeleteItemFromObject(json, "test-dependencies");
        }
        test_deps = cJSON_CreateObject();
        
        // Add test direct dependencies
        cJSON *test_direct = cJSON_CreateObject();
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            char key[MAX_KEY_LENGTH];
            snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
            cJSON_AddStringToObject(test_direct, key, pkg->version);
        }
        cJSON_AddItemToObject(test_deps, "direct", test_direct);
        
        // Add test indirect dependencies
        cJSON *test_indirect = cJSON_CreateObject();
        for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
            char key[MAX_KEY_LENGTH];
            snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
            cJSON_AddStringToObject(test_indirect, key, pkg->version);
        }
        cJSON_AddItemToObject(test_deps, "indirect", test_indirect);
        
        cJSON_AddItemToObject(json, "test-dependencies", test_deps);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        // Update dependencies object
        if (elm_json->package_dependencies) {
            cJSON *deps = cJSON_GetObjectItem(json, "dependencies");
            if (deps) {
                cJSON_DeleteItemFromObject(json, "dependencies");
            }
            deps = cJSON_CreateObject();
            for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                Package *pkg = &elm_json->package_dependencies->packages[i];
                char key[MAX_KEY_LENGTH];
                snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
                cJSON_AddStringToObject(deps, key, pkg->version);
            }
            cJSON_AddItemToObject(json, "dependencies", deps);
        }
        
        // Update test-dependencies object
        if (elm_json->package_test_dependencies) {
            cJSON *test_deps = cJSON_GetObjectItem(json, "test-dependencies");
            if (test_deps) {
                cJSON_DeleteItemFromObject(json, "test-dependencies");
            }
            test_deps = cJSON_CreateObject();
            for (int i = 0; i < elm_json->package_test_dependencies->count; i++) {
                Package *pkg = &elm_json->package_test_dependencies->packages[i];
                char key[MAX_KEY_LENGTH];
                snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
                cJSON_AddStringToObject(test_deps, key, pkg->version);
            }
            cJSON_AddItemToObject(json, "test-dependencies", test_deps);
        }
    }
    
    /* Use custom formatter instead of cJSON_Print; write atomically */
    bool result = elm_json_write_formatted_atomic(json, filepath);
    cJSON_Delete(json);
    
    return result;
}

void elm_json_free(ElmJson *elm_json) {
    if (!elm_json) return;
    
    arena_free(elm_json->elm_version);
    package_map_free(elm_json->dependencies_direct);
    package_map_free(elm_json->dependencies_indirect);
    package_map_free(elm_json->dependencies_test_direct);
    package_map_free(elm_json->dependencies_test_indirect);
    
    if (elm_json->package_name) arena_free(elm_json->package_name);
    if (elm_json->package_version) arena_free(elm_json->package_version);
    if (elm_json->package_dependencies) package_map_free(elm_json->package_dependencies);
    if (elm_json->package_test_dependencies) package_map_free(elm_json->package_test_dependencies);
    
    arena_free(elm_json);
}

/* Dependency promotion */
PromotionType elm_json_find_package(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || !author || !name) return PROMOTION_NONE;
    
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        // Check if already in direct dependencies
        if (package_map_find(elm_json->dependencies_direct, author, name)) {
            return PROMOTION_NONE;
        }
        
        // Check if in indirect dependencies (indirect -> direct)
        if (package_map_find(elm_json->dependencies_indirect, author, name)) {
            return PROMOTION_INDIRECT_TO_DIRECT;
        }
        
        // Check if in test direct dependencies (test -> direct)
        if (package_map_find(elm_json->dependencies_test_direct, author, name)) {
            return PROMOTION_TEST_TO_DIRECT;
        }
        
        // Check if in test indirect dependencies (test indirect -> test direct)
        if (package_map_find(elm_json->dependencies_test_indirect, author, name)) {
            return PROMOTION_TEST_INDIRECT_TO_TEST_DIRECT;
        }
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        // For packages, check if already in dependencies
        if (package_map_find(elm_json->package_dependencies, author, name)) {
            return PROMOTION_NONE;
        }
        
        // Check if in test dependencies
        if (package_map_find(elm_json->package_test_dependencies, author, name)) {
            return PROMOTION_TEST_TO_DIRECT;
        }
    }
    
    return PROMOTION_NONE;
}

bool elm_json_promote_package(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || !author || !name) return false;
    
    PromotionType promotion = elm_json_find_package(elm_json, author, name);
    
    if (promotion == PROMOTION_NONE) {
        return false;  // Package not found or already in direct
    }
    
    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        Package *pkg = NULL;
        
        switch (promotion) {
            case PROMOTION_INDIRECT_TO_DIRECT:
                // Move from indirect to direct
                pkg = package_map_find(elm_json->dependencies_indirect, author, name);
                if (pkg) {
                    package_map_add(elm_json->dependencies_direct, author, name, pkg->version);
                    package_map_remove(elm_json->dependencies_indirect, author, name);
                    log_progress("Promoted %s/%s from indirect to direct dependencies", author, name);
                    return true;
                }
                break;
                
            case PROMOTION_TEST_TO_DIRECT:
                // Move from test-direct to direct
                pkg = package_map_find(elm_json->dependencies_test_direct, author, name);
                if (pkg) {
                    package_map_add(elm_json->dependencies_direct, author, name, pkg->version);
                    package_map_remove(elm_json->dependencies_test_direct, author, name);
                    log_progress("Promoted %s/%s from test to direct dependencies", author, name);
                    return true;
                }
                break;
                
            case PROMOTION_TEST_INDIRECT_TO_TEST_DIRECT:
                // Move from test-indirect to test-direct
                pkg = package_map_find(elm_json->dependencies_test_indirect, author, name);
                if (pkg) {
                    package_map_add(elm_json->dependencies_test_direct, author, name, pkg->version);
                    package_map_remove(elm_json->dependencies_test_indirect, author, name);
                    log_progress("Promoted %s/%s from test-indirect to test-direct dependencies", author, name);
                    return true;
                }
                break;
                
            default:
                return false;
        }
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        if (promotion == PROMOTION_TEST_TO_DIRECT) {
            Package *pkg = package_map_find(elm_json->package_test_dependencies, author, name);
            if (pkg) {
                package_map_add(elm_json->package_dependencies, author, name, pkg->version);
                package_map_remove(elm_json->package_test_dependencies, author, name);
                log_progress("Promoted %s/%s from test to main dependencies", author, name);
                return true;
            }
        }
    }
    
    return false;
}
