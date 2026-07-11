#include "dependency_usage.h"

#include "alloc.h"
#include "ast/skeleton.h"
#include "cache.h"
#include "constants.h"
#include "elm_project.h"
#include "shared/log.h"
#include "terminal_colors.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    Package *package;
    char **modules;
    int module_count;
    bool used;
} DirectDependencyUsage;

static bool is_implicitly_used(const Package *pkg) {
    if (!pkg || !pkg->author || !pkg->name || strcmp(pkg->author, "elm") != 0) {
        return false;
    }

    return strcmp(pkg->name, "core") == 0 || strcmp(pkg->name, "json") == 0;
}

static void free_string_array(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        arena_free(items[i]);
    }
    arena_free(items);
}

static void free_usage_modules(DirectDependencyUsage *usages, int count) {
    if (!usages) return;
    for (int i = 0; i < count; i++) {
        free_string_array(usages[i].modules, usages[i].module_count);
    }
    arena_free(usages);
}

static bool get_project_directory(const char *elm_json_path, char *out, size_t out_size) {
    if (!elm_json_path || !out || out_size == 0) return false;
    if (!realpath(elm_json_path, out)) return false;

    char *last_slash = strrchr(out, '/');
    char *last_backslash = strrchr(out, '\\');
    if (!last_slash || (last_backslash && last_backslash > last_slash)) {
        last_slash = last_backslash;
    }
    if (!last_slash || last_slash == out) return false;
    *last_slash = '\0';
    return true;
}

static bool path_is_absolute(const char *path) {
    if (!path || path[0] == '\0') return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    return path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static bool load_direct_dependency_modules(
    const ElmJson *elm_json,
    CacheConfig *cache,
    DirectDependencyUsage **out_usages,
    int *out_count
) {
    *out_usages = NULL;
    *out_count = 0;

    if (!elm_json->dependencies_direct || elm_json->dependencies_direct->count == 0) {
        return true;
    }

    int count = elm_json->dependencies_direct->count;
    DirectDependencyUsage *usages = arena_calloc((size_t)count, sizeof(DirectDependencyUsage));
    if (!usages) return false;

    for (int i = 0; i < count; i++) {
        Package *pkg = &elm_json->dependencies_direct->packages[i];
        usages[i].package = pkg;
        usages[i].used = is_implicitly_used(pkg);

        if (usages[i].used) continue;
        if (!pkg->author || !pkg->name || !pkg->version ||
            !cache_package_exists(cache, pkg->author, pkg->name, pkg->version)) {
            log_debug("Skipping dependency usage analysis: package metadata is unavailable for %s/%s",
                      pkg->author ? pkg->author : "(unknown)",
                      pkg->name ? pkg->name : "(unknown)");
            free_usage_modules(usages, count);
            return false;
        }

        char *pkg_path = cache_get_package_path(cache, pkg->author, pkg->name, pkg->version);
        if (!pkg_path) {
            free_usage_modules(usages, count);
            return false;
        }

        char package_elm_json[MAX_PATH_LENGTH];
        int path_len = snprintf(package_elm_json, sizeof(package_elm_json),
                                "%s/elm.json", pkg_path);
        arena_free(pkg_path);
        if (path_len < 0 || path_len >= (int)sizeof(package_elm_json)) {
            free_usage_modules(usages, count);
            return false;
        }

        usages[i].modules = elm_parse_exposed_modules(package_elm_json, &usages[i].module_count);
        if (!usages[i].modules || usages[i].module_count <= 0) {
            log_debug("Skipping dependency usage analysis: exposed module metadata is unavailable in %s",
                      package_elm_json);
            free_usage_modules(usages, count);
            return false;
        }
    }

    *out_usages = usages;
    *out_count = count;
    return true;
}

static void mark_imported_dependencies(
    DirectDependencyUsage *usages,
    int usage_count,
    const SkeletonModule *module
) {
    for (int import_index = 0; import_index < module->imports_count; import_index++) {
        const char *import_name = module->imports[import_index].module_name;
        if (!import_name) continue;

        for (int usage_index = 0; usage_index < usage_count; usage_index++) {
            DirectDependencyUsage *usage = &usages[usage_index];
            if (usage->used) continue;

            for (int module_index = 0; module_index < usage->module_count; module_index++) {
                if (usage->modules[module_index] &&
                    strcmp(usage->modules[module_index], import_name) == 0) {
                    usage->used = true;
                    break;
                }
            }
        }
    }
}

static bool scan_application_sources(
    const char *elm_json_path,
    DirectDependencyUsage *usages,
    int usage_count
) {
    int source_dir_count = 0;
    char **source_dirs = elm_parse_source_directories(elm_json_path, &source_dir_count);
    if (!source_dirs || source_dir_count <= 0) {
        free_string_array(source_dirs, source_dir_count);
        return false;
    }

    char project_dir[MAX_PATH_LENGTH];
    if (!get_project_directory(elm_json_path, project_dir, sizeof(project_dir))) {
        free_string_array(source_dirs, source_dir_count);
        return false;
    }

    int file_capacity = INITIAL_FILE_CAPACITY;
    int file_count = 0;
    char **files = arena_malloc((size_t)file_capacity * sizeof(char *));
    if (!files) {
        free_string_array(source_dirs, source_dir_count);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < source_dir_count && ok; i++) {
        char source_dir[MAX_PATH_LENGTH];
        int path_len;
        if (path_is_absolute(source_dirs[i])) {
            path_len = snprintf(source_dir, sizeof(source_dir), "%s", source_dirs[i]);
        } else {
            path_len = snprintf(source_dir, sizeof(source_dir), "%s/%s",
                                project_dir, source_dirs[i]);
        }
        if (path_len < 0 || path_len >= (int)sizeof(source_dir)) {
            ok = false;
            break;
        }

        struct stat st;
        if (stat(source_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            log_debug("Skipping dependency usage analysis: source directory is unavailable: %s",
                      source_dir);
            ok = false;
            break;
        }

        elm_collect_elm_files(source_dir, &files, &file_count, &file_capacity);
    }

    for (int i = 0; i < file_count && ok; i++) {
        SkeletonModule *module = skeleton_parse(files[i]);
        if (!module) {
            log_debug("Skipping dependency usage analysis: cannot parse %s", files[i]);
            ok = false;
            break;
        }
        mark_imported_dependencies(usages, usage_count, module);
        skeleton_free(module);
    }

    free_string_array(files, file_count);
    free_string_array(source_dirs, source_dir_count);
    return ok;
}

bool dependency_usage_find_unused_direct(
    const char *elm_json_path,
    const ElmJson *elm_json,
    CacheConfig *cache,
    PackageMap **out_unused
) {
    if (out_unused) *out_unused = NULL;
    if (!elm_json_path || !elm_json || !cache || !out_unused ||
        elm_json->type != ELM_PROJECT_APPLICATION) {
        return false;
    }

    DirectDependencyUsage *usages = NULL;
    int usage_count = 0;
    if (!load_direct_dependency_modules(elm_json, cache, &usages, &usage_count)) {
        return false;
    }

    if (!scan_application_sources(elm_json_path, usages, usage_count)) {
        free_usage_modules(usages, usage_count);
        return false;
    }

    PackageMap *unused = package_map_create();
    if (!unused) {
        free_usage_modules(usages, usage_count);
        return false;
    }

    for (int i = 0; i < usage_count; i++) {
        Package *pkg = usages[i].package;
        if (!usages[i].used &&
            !package_map_add(unused, pkg->author, pkg->name, pkg->version)) {
            package_map_free(unused);
            free_usage_modules(usages, usage_count);
            return false;
        }
    }

    free_usage_modules(usages, usage_count);
    if (unused->count == 0) {
        package_map_free(unused);
        return true;
    }

    *out_unused = unused;
    return true;
}

static void print_redundant_dependency_entries(const PackageMap *packages) {
    if (!packages) return;

    for (int i = 0; i < packages->count; i++) {
        Package *pkg = &packages->packages[i];
        fprintf(stderr, "    %s✗%s %s/%s  %s\n",
                ANSI_RED, ANSI_RESET, pkg->author, pkg->name, pkg->version);
    }
}

void dependency_usage_report_redundant(
    const PackageMap *unused_direct,
    const PackageMap *orphaned_indirect
) {
    int unused_count = unused_direct ? unused_direct->count : 0;
    int orphaned_count = orphaned_indirect ? orphaned_indirect->count : 0;
    if (unused_count == 0 && orphaned_count == 0) return;

    fprintf(stderr, "\n%s-- REDUNDANT DEPENDENCIES ------------------------------------------------------%s\n\n",
            ANSI_DULL_CYAN, ANSI_RESET);

    if (unused_count > 0) {
        fprintf(stderr, "The following direct dependencies are not imported by application source files:\n\n");
        print_redundant_dependency_entries(unused_direct);
        fprintf(stderr, "\n");
    }

    if (orphaned_count > 0) {
        fprintf(stderr, "The following indirect dependencies are not required by any direct dependency:\n\n");
        print_redundant_dependency_entries(orphaned_indirect);
        fprintf(stderr, "\n");
    }
}
