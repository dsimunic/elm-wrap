#include "docs.h"
#include "elm_docs.h"
#include "docs_json.h"
#include "dependency_cache.h"
#include "path_util.h"
#include "../../../alloc.h"
#include "../../../global_context.h"
#include "../../../cache.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <ctype.h>

static void print_docs_usage(void) {
    printf("Usage: %s publish docs PATH\n", global_context_program_name());
    printf("\n");
    printf("Generate documentation JSON for an Elm package.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PATH               Path to package directory containing elm.json and src/\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  -v, --verbose      Show verbose output including skipped modules\n");
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

/* Check if module already exists in the list */
static bool exposed_modules_contains(const ExposedModules *em, const char *module) {
    for (int i = 0; i < em->count; i++) {
        if (strcmp(em->modules[i], module) == 0) {
            return true;
        }
    }
    return false;
}

static void exposed_modules_add(ExposedModules *em, const char *module) {
    /* Skip duplicates */
    if (exposed_modules_contains(em, module)) {
        fprintf(stderr, "Warning: Duplicate exposed module '%s' in elm.json (ignoring)\n", module);
        return;
    }

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

/* Validate that file path matches module name according to Elm convention */
static bool validate_module_path(const char *filepath, const char *module_name, const char *src_dir) {
    /* Convert module name to expected relative path */
    /* e.g., "Eth.Sentry.Event" -> "Eth/Sentry/Event.elm" */
    size_t module_len = strlen(module_name);
    char *expected_rel_path = arena_malloc(module_len + 5); /* +5 for ".elm\0" */

    /* Copy module name and replace dots with slashes */
    const char *src = module_name;
    char *dst = expected_rel_path;
    while (*src) {
        if (*src == '.') {
            *dst = '/';
        } else {
            *dst = *src;
        }
        src++;
        dst++;
    }
    strcpy(dst, ".elm");

    /* Extract relative path from filepath */
    /* filepath is like ".../src/Eth/Sentry/Event.elm" */
    /* We need to find where src_dir ends in filepath */
    const char *rel_path = NULL;

    /* Find src_dir within filepath */
    const char *src_pos = strstr(filepath, src_dir);
    if (src_pos) {
        /* Skip past src_dir and the trailing slash */
        rel_path = src_pos + strlen(src_dir);
        if (*rel_path == '/') {
            rel_path++;
        }
    }

    if (!rel_path) {
        /* Couldn't find src_dir in filepath - shouldn't happen */
        arena_free(expected_rel_path);
        return false;
    }

    /* Compare the paths */
    bool match = strcmp(rel_path, expected_rel_path) == 0;

    arena_free(expected_rel_path);
    return match;
}

/**
 * Build file list from exposed modules instead of scanning directory.
 * Converts each exposed module name to its expected file path and checks existence.
 *
 * @param exposed The list of exposed module names from elm.json
 * @param src_path The path to the src/ directory
 * @param files Output FileList to populate
 * @return true if all exposed modules have corresponding files, false if any missing
 */
static bool find_files_for_exposed_modules(
    const ExposedModules *exposed,
    const char *src_path,
    FileList *files
) {
    bool all_found = true;

    for (int i = 0; i < exposed->count; i++) {
        const char *module_name = exposed->modules[i];

        /* Convert module name to relative path: "Eth.Sentry.Event" -> "Eth/Sentry/Event.elm" */
        char *rel_path = module_name_to_file_path(module_name);

        /* Build full path: src_path + "/" + rel_path */
        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", src_path, rel_path);
        arena_free(rel_path);

        /* Check if file exists */
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            file_list_add(files, full_path);
        } else {
            fprintf(stderr, "Error: Exposed module '%s' not found at expected path: %s\n",
                    module_name, full_path);
            all_found = false;
        }
    }

    return all_found;
}

/* Comparison function for sorting modules alphabetically by name */
static int compare_modules(const void *a, const void *b) {
    const ElmModuleDocs *mod_a = (const ElmModuleDocs *)a;
    const ElmModuleDocs *mod_b = (const ElmModuleDocs *)b;
    return strcmp(mod_a->name, mod_b->name);
}

/* Process a list of .elm files (all files are pre-filtered as exposed modules) */
static int process_files(FileList *files, const char *base_path, bool verbose __attribute__((unused)), ExposedModules *exposed) {
    if (files->count == 0) {
        fprintf(stderr, "Warning: No .elm files found\n");
        return 0;
    }

    fprintf(stderr, "Processing %d exposed module(s)\n", exposed->count);

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
        if (dep_cache) dependency_cache_free(dep_cache);
        if (cache_config) cache_config_free(cache_config);
        return 1;
    }

    /* Construct src directory path for validation */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/src", base_path);

    /* Parse all files - all files are pre-filtered as exposed modules */
    int doc_index = 0;
    for (int i = 0; i < files->count; i++) {
        fprintf(stderr, "Processing: %s\n", files->paths[i]);

        if (!parse_elm_file(files->paths[i], &all_docs[doc_index], dep_cache)) {
            fprintf(stderr, "Warning: Failed to parse %s\n", files->paths[i]);
        } else {
            /* Sanity check: validate that file path matches module name */
            /* This should always pass since we looked up files by module name */
            if (!validate_module_path(files->paths[i], all_docs[doc_index].name, src_path)) {
                fprintf(stderr, "Warning: Module name '%s' doesn't match file path: %s\n",
                        all_docs[doc_index].name, files->paths[i]);
                fprintf(stderr, "This may indicate a module declaration mismatch.\n");
                /* Continue processing - this is now a warning, not a fatal error */
            }

            fprintf(stderr, "Successfully parsed: %s (Module: %s, %d values, %d aliases, %d unions, %d binops)\n",
                    files->paths[i], all_docs[doc_index].name,
                    all_docs[doc_index].values_count, all_docs[doc_index].aliases_count,
                    all_docs[doc_index].unions_count, all_docs[doc_index].binops_count);
            doc_index++;
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
    bool verbose = false;
    const char *package_path = NULL;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_docs_usage();
            return 0;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (package_path == NULL) {
            package_path = argv[i];
        } else {
            fprintf(stderr, "Error: Unexpected argument: %s\n\n", argv[i]);
            print_docs_usage();
            return 1;
        }
    }

    if (package_path == NULL) {
        fprintf(stderr, "Error: Missing required argument <PATH>\n\n");
        print_docs_usage();
        return 1;
    }

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

    // Parse elm.json to get exposed modules
    ExposedModules exposed;
    exposed_modules_init(&exposed);

    if (!parse_elm_json(elm_json_path, &exposed)) {
        fprintf(stderr, "ERROR: elm.json is missing required 'exposed-modules' field or has invalid format\n");
        fprintf(stderr, "Package elm.json must contain 'exposed-modules' field with at least one module\n");
        exposed_modules_free(&exposed);
        return 1;
    }

    fprintf(stderr, "Found elm.json with %d exposed module(s)\n", exposed.count);

    // Check for src directory
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/src", package_path);
    if (stat(src_path, &st) != 0) {
        fprintf(stderr, "Error: No src/ directory found in %s\n", package_path);
        exposed_modules_free(&exposed);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s/src is not a directory\n", package_path);
        exposed_modules_free(&exposed);
        return 1;
    }

    // Build file list from exposed modules (instead of scanning all files)
    FileList files;
    file_list_init(&files);

    if (!find_files_for_exposed_modules(&exposed, src_path, &files)) {
        fprintf(stderr, "Error: One or more exposed modules not found at expected paths\n");
        file_list_free(&files);
        exposed_modules_free(&exposed);
        return 101;
    }

    int result = process_files(&files, package_path, verbose, &exposed);
    file_list_free(&files);
    exposed_modules_free(&exposed);

    return result;
}
