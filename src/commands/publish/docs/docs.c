#include "docs.h"
#include "elm_docs.h"
#include "docs_json.h"
#include "dependency_cache.h"
#include "../../../alloc.h"
#include "../../../progname.h"
#include "../../../cache.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <ctype.h>

static void print_docs_usage(void) {
    printf("Usage: %s publish docs <PATH>\n", program_name);
    printf("\n");
    printf("Generate documentation JSON for an Elm package.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  <PATH>             Path to package directory containing elm.json and src/\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
}

/* Data structure for exposed modules */
typedef struct {
    char **modules;
    int count;
    int capacity;
} ExposedModules;

static void exposed_modules_init(ExposedModules *em) {
    em->modules = NULL;
    em->count = 0;
    em->capacity = 0;
}

static void exposed_modules_add(ExposedModules *em, const char *module) {
    if (em->count >= em->capacity) {
        em->capacity = em->capacity == 0 ? 16 : em->capacity * 2;
        em->modules = arena_realloc(em->modules, em->capacity * sizeof(char*));
    }
    em->modules[em->count++] = arena_strdup(module);
}

static void exposed_modules_free(ExposedModules *em) {
    for (int i = 0; i < em->count; i++) {
        arena_free(em->modules[i]);
    }
    arena_free(em->modules);
}

static bool exposed_modules_contains(ExposedModules *em, const char *module) {
    for (int i = 0; i < em->count; i++) {
        if (strcmp(em->modules[i], module) == 0) {
            return true;
        }
    }
    return false;
}

/* Parse elm.json to extract exposed-modules */
static bool parse_elm_json(const char *elm_json_path, ExposedModules *em) {
    FILE *f = fopen(elm_json_path, "r");
    if (!f) {
        return false;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = arena_malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return false;
    }

    fread(content, 1, fsize, f);
    content[fsize] = 0;
    fclose(f);

    /* Find "exposed-modules" key */
    char *exposed = strstr(content, "\"exposed-modules\"");
    if (!exposed) {
        arena_free(content);
        return false;
    }

    /* Skip past the key to find the value */
    char *value_start = exposed + strlen("\"exposed-modules\"");
    while (*value_start && (*value_start == ':' || isspace(*value_start))) {
        value_start++;
    }

    /* Check if it's an object (categorized) or array (simple) */
    bool is_categorized = (*value_start == '{');

    /* Find the opening bracket of first array */
    char *bracket = strchr(value_start, '[');
    if (!bracket) {
        arena_free(content);
        return false;
    }

    /* Parse all arrays (for categorized format, there may be multiple) */
    char *search_pos = bracket;
    char *end_marker = is_categorized ? strchr(value_start, '}') : strchr(bracket, ']');

    while (search_pos && search_pos < end_marker) {
        /* Parse module names within this array */
        char *p = search_pos + 1;
        while (*p) {
            /* Skip whitespace */
            while (*p && isspace(*p)) p++;

            /* Check for end of this array */
            if (*p == ']') break;

            /* Find opening quote */
            if (*p == '"') {
                p++;
                char *start = p;
                /* Find closing quote */
                while (*p && *p != '"') p++;
                if (*p == '"') {
                    /* Extract module name */
                    int len = p - start;
                    char *module = arena_malloc(len + 1);
                    strncpy(module, start, len);
                    module[len] = 0;
                    exposed_modules_add(em, module);
                    arena_free(module);
                    p++;
                }
            }

            /* Skip to next element or end */
            while (*p && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
        }

        /* For categorized format, find next array if any */
        if (is_categorized) {
            search_pos = strchr(p, '[');
        } else {
            /* For simple array format, we're done */
            break;
        }
    }

    arena_free(content);
    return em->count > 0;
}

/* Dynamic array for collecting file paths */
typedef struct {
    char **paths;
    int count;
    int capacity;
} FileList;

static void file_list_init(FileList *list) {
    list->paths = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void file_list_add(FileList *list, const char *path) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        list->paths = arena_realloc(list->paths, list->capacity * sizeof(char*));
    }
    list->paths[list->count++] = arena_strdup(path);
}

static void file_list_free(FileList *list) {
    for (int i = 0; i < list->count; i++) {
        arena_free(list->paths[i]);
    }
    arena_free(list->paths);
}

/* Check if file has .elm extension */
static int is_elm_file(const char *filename) {
    size_t len = strlen(filename);
    return len > 4 && strcmp(filename + len - 4, ".elm") == 0;
}

/* Recursively find all .elm files in a directory */
static void find_elm_files_recursive(const char *dir_path, FileList *files) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(filepath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                /* Recursively search subdirectories */
                find_elm_files_recursive(filepath, files);
            } else if (S_ISREG(st.st_mode) && is_elm_file(entry->d_name)) {
                /* Add .elm file to list */
                file_list_add(files, filepath);
            }
        }
    }

    closedir(dir);
}

/* Comparison function for sorting modules alphabetically by name */
static int compare_modules(const void *a, const void *b) {
    const ElmModuleDocs *mod_a = (const ElmModuleDocs *)a;
    const ElmModuleDocs *mod_b = (const ElmModuleDocs *)b;
    return strcmp(mod_a->name, mod_b->name);
}

/* Process a list of .elm files */
static int process_files(FileList *files, const char *base_path) {
    if (files->count == 0) {
        fprintf(stderr, "Warning: No .elm files found\n");
        return 0;
    }

    /* Try to read elm.json to get exposed modules */
    ExposedModules exposed;
    exposed_modules_init(&exposed);

    char elm_json_path[1024];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", base_path);

    bool has_elm_json = parse_elm_json(elm_json_path, &exposed);
    if (has_elm_json) {
        fprintf(stderr, "Found elm.json with %d exposed module(s)\n", exposed.count);
    } else {
        fprintf(stderr, "No elm.json found or failed to parse, including all modules\n");
    }

    /* Initialize dependency cache */
    CacheConfig *cache_config = cache_config_init();
    DependencyCache *dep_cache = NULL;
    if (cache_config && cache_config->elm_home) {
        dep_cache = dependency_cache_create(cache_config->elm_home, base_path);
        fprintf(stderr, "Initialized dependency cache with ELM_HOME: %s\n", cache_config->elm_home);
    } else {
        fprintf(stderr, "Warning: Could not initialize dependency cache (ELM_HOME not found)\n");
    }

    /* Allocate array for all module docs */
    ElmModuleDocs *all_docs = arena_calloc(files->count, sizeof(ElmModuleDocs));
    if (!all_docs) {
        exposed_modules_free(&exposed);
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    /* Parse all files */
    int doc_index = 0;
    for (int i = 0; i < files->count; i++) {
        fprintf(stderr, "Processing: %s\n", files->paths[i]);

        if (!parse_elm_file(files->paths[i], &all_docs[doc_index], dep_cache)) {
            fprintf(stderr, "Warning: Failed to parse %s\n", files->paths[i]);
        } else {
            /* Check if module is exposed (if we have elm.json) */
            bool is_exposed = !has_elm_json ||
                             exposed_modules_contains(&exposed, all_docs[doc_index].name);

            if (is_exposed) {
                fprintf(stderr, "Successfully parsed: %s (Module: %s)\n",
                        files->paths[i], all_docs[doc_index].name);
                doc_index++;
            } else {
                fprintf(stderr, "Skipping non-exposed module: %s\n", all_docs[doc_index].name);
                free_elm_docs(&all_docs[doc_index]);
            }
        }
    }

    /* Sort modules alphabetically by name */
    if (doc_index > 1) {
        qsort(all_docs, doc_index, sizeof(ElmModuleDocs), compare_modules);
    }

    /* Print docs.json to stdout */
    fprintf(stderr, "\nGenerating documentation for %d module(s)...\n", doc_index);
    print_docs_json(all_docs, doc_index);

    /* Cleanup */
    for (int i = 0; i < doc_index; i++) {
        free_elm_docs(&all_docs[i]);
    }
    arena_free(all_docs);
    exposed_modules_free(&exposed);

    /* Free dependency cache */
    if (dep_cache) {
        dependency_cache_free(dep_cache);
    }
    if (cache_config) {
        cache_config_free(cache_config);
    }

    return 0;
}

int cmd_publish_docs(int argc, char *argv[]) {
    // Check for help flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_docs_usage();
            return 0;
        }
    }

    if (argc < 2) {
        fprintf(stderr, "Error: Missing required argument <PATH>\n\n");
        print_docs_usage();
        return 1;
    }

    const char *package_path = argv[1];

    // Check if the path exists
    struct stat st;
    if (stat(package_path, &st) != 0) {
        fprintf(stderr, "Error: Could not access %s\n", package_path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s is not a directory\n", package_path);
        return 1;
    }

    // Check for elm.json
    char elm_json_path[1024];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", package_path);
    if (stat(elm_json_path, &st) != 0) {
        fprintf(stderr, "Error: No elm.json found in %s\n", package_path);
        return 1;
    }

    // Check for src directory
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/src", package_path);
    if (stat(src_path, &st) != 0) {
        fprintf(stderr, "Error: No src/ directory found in %s\n", package_path);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s/src is not a directory\n", package_path);
        return 1;
    }

    // Find all .elm files in src/
    FileList files;
    file_list_init(&files);
    find_elm_files_recursive(src_path, &files);

    int result = process_files(&files, package_path);
    file_list_free(&files);

    return result;
}
