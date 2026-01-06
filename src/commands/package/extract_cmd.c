#include "extract_cmd.h"
#include "package_common.h"
#include "install_local_dev.h"
#include "../../install.h"
#include "../../install_env.h"
#include "../../global_context.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../shared/log.h"
#include "../../fileutil.h"
#include "../../elm_json.h"
#include "../../elm_project.h"
#include "../../embedded_archive.h"
#include "../../ast/skeleton.h"
#include "../../dyn_array.h"
#include "../../vendor/cJSON.h"
#include "../../terminal_colors.h"
#include "../../cache.h"
#include "../../plural.h"
#include "../../messages.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

/* ============================================================================
 * Data structures
 * ========================================================================== */

typedef struct {
    char **abs_paths;       /* Absolute source paths */
    char **dest_relatives;  /* Destination paths relative to target/src */
    int  *source_indices;   /* Index into PATH args that produced this file */
    int count;
    int capacity;
} SelectedFiles;

typedef struct {
    char *importing_file_abs;
    char *importing_module_name;
    char *imported_module_name;
    char *imported_file_abs;
} ExtractViolation;

typedef struct {
    ExtractViolation *violations;
    int count;
    int capacity;
} ViolationList;

typedef struct {
    char *module_name;
    char *package_name; /* author/name */
} ExternalModuleOwner;

typedef struct {
    ExternalModuleOwner *items;
    int count;
    int capacity;
} ExternalModuleOwnerMap;

/* ============================================================================
 * Usage and helpers
 * ========================================================================== */

static void print_extract_usage(void) {
    printf("Usage: %s package extract PACKAGE TARGET_PATH PATH [PATH...]\n",
           global_context_program_name());
    printf("\n");
    printf("Extract Elm source from an application into a new package.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  PACKAGE       Package name (author/name or author/name@version)\n");
    printf("  TARGET_PATH   Directory where new package will be created\n");
    printf("  PATH          One or more source files or directories to extract\n");
    printf("\n");
    printf("Multiple paths can be specified to extract both a head module and its\n");
    printf("submodule directory. For example:\n");
    printf("  %s package extract me/pkg ../pkg src/Foo.elm src/Foo\n",
           global_context_program_name());
    printf("\n");
    printf("Options:\n");
    printf("  -y, --yes           Skip confirmation prompt\n");
    printf("  --no-local-dev      Do not register as a local-dev dependency\n");
    printf("  -h, --help          Show this help message\n");
}

static bool path_is_elm_file(const char *path) {
    size_t len = strlen(path);
    return len > 4 && strcmp(path + len - 4, ".elm") == 0;
}

static int compare_packages_by_name(const void *a, const void *b) {
    const Package *pa = (const Package *)a;
    const Package *pb = (const Package *)b;

    int author_cmp = strcmp(pa->author, pb->author);
    if (author_cmp != 0) return author_cmp;
    return strcmp(pa->name, pb->name);
}

static int compare_module_owner_by_module(const void *a, const void *b) {
    const ExternalModuleOwner *ma = (const ExternalModuleOwner *)a;
    const ExternalModuleOwner *mb = (const ExternalModuleOwner *)b;
    return strcmp(ma->module_name, mb->module_name);
}

static int compare_string_ptrs(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;

    if (!sa && !sb) return 0;
    if (!sa) return -1;
    if (!sb) return 1;
    return strcmp(sa, sb);
}

static char *format_author_name(const char *author, const char *name) {
    if (!author || !name) return NULL;
    size_t len = strlen(author) + 1 + strlen(name) + 1;
    char *out = arena_malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s/%s", author, name);
    return out;
}

static bool package_list_contains(Package *pkgs, int count, const char *author, const char *name) {
    if (!pkgs || !author || !name) return false;
    for (int i = 0; i < count; i++) {
        if (pkgs[i].author && pkgs[i].name &&
            strcmp(pkgs[i].author, author) == 0 && strcmp(pkgs[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void collect_packages_from_map(PackageMap *map, Package **out_pkgs, int *out_count, int *out_capacity) {
    if (!map || !out_pkgs || !out_count || !out_capacity) return;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];
        if (!pkg->author || !pkg->name || !pkg->version) continue;
        if (package_list_contains(*out_pkgs, *out_count, pkg->author, pkg->name)) continue;

        DYNARRAY_ENSURE_CAPACITY(*out_pkgs, *out_count, *out_capacity, Package);
        (*out_pkgs)[*out_count].author = arena_strdup(pkg->author);
        (*out_pkgs)[*out_count].name = arena_strdup(pkg->name);
        (*out_pkgs)[*out_count].version = arena_strdup(pkg->version);
        (*out_count)++;
    }
}

static void external_module_owner_map_init(ExternalModuleOwnerMap *map) {
    if (!map) return;
    map->count = 0;
    map->capacity = INITIAL_MEDIUM_CAPACITY;
    map->items = arena_malloc(map->capacity * sizeof(ExternalModuleOwner));
}

static bool external_module_owner_map_contains_module(const ExternalModuleOwnerMap *map, const char *module_name) {
    if (!map || !map->items || !module_name) return false;
    for (int i = 0; i < map->count; i++) {
        if (map->items[i].module_name && strcmp(map->items[i].module_name, module_name) == 0) {
            return true;
        }
    }
    return false;
}

static void external_module_owner_map_add(ExternalModuleOwnerMap *map, const char *module_name, char *package_name) {
    if (!map || !module_name || !package_name) return;
    if (external_module_owner_map_contains_module(map, module_name)) return;

    DYNARRAY_ENSURE_CAPACITY(map->items, map->count, map->capacity, ExternalModuleOwner);
    map->items[map->count].module_name = arena_strdup(module_name);
    map->items[map->count].package_name = package_name;
    map->count++;
}

static void external_module_owner_map_sort(ExternalModuleOwnerMap *map) {
    if (!map || !map->items || map->count <= 1) return;
    qsort(map->items, map->count, sizeof(ExternalModuleOwner), compare_module_owner_by_module);
}

static const char *external_module_owner_map_find(const ExternalModuleOwnerMap *map, const char *module_name) {
    if (!map || !map->items || map->count == 0 || !module_name) return NULL;

    ExternalModuleOwner key;
    key.module_name = (char *)module_name;
    key.package_name = NULL;

    ExternalModuleOwner *found = bsearch(&key, map->items, map->count,
                                         sizeof(ExternalModuleOwner), compare_module_owner_by_module);
    return found ? found->package_name : NULL;
}

static bool build_external_module_owner_map_from_app(const ElmJson *app_json, ExternalModuleOwnerMap *out_map) {
    if (!app_json || app_json->type != ELM_PROJECT_APPLICATION || !out_map) return false;

    CacheConfig *cache = cache_config_init();
    if (!cache) {
        return false;
    }

    int pkgs_capacity = INITIAL_MEDIUM_CAPACITY;
    int pkgs_count = 0;
    Package *pkgs = arena_malloc(pkgs_capacity * sizeof(Package));
    if (!pkgs) {
        cache_config_free(cache);
        return false;
    }

    collect_packages_from_map(app_json->dependencies_direct, &pkgs, &pkgs_count, &pkgs_capacity);
    collect_packages_from_map(app_json->dependencies_indirect, &pkgs, &pkgs_count, &pkgs_capacity);
    collect_packages_from_map(app_json->dependencies_test_direct, &pkgs, &pkgs_count, &pkgs_capacity);
    collect_packages_from_map(app_json->dependencies_test_indirect, &pkgs, &pkgs_count, &pkgs_capacity);

    if (pkgs_count > 1) {
        qsort(pkgs, pkgs_count, sizeof(Package), compare_packages_by_name);
    }

    external_module_owner_map_init(out_map);
    if (!out_map->items) {
        cache_config_free(cache);
        return false;
    }

    for (int i = 0; i < pkgs_count; i++) {
        const char *author = pkgs[i].author;
        const char *name = pkgs[i].name;
        const char *version = pkgs[i].version;
        if (!author || !name || !version) continue;

        char *pkg_path = cache_get_package_path(cache, author, name, version);
        if (!pkg_path) continue;

        struct stat st;
        if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            arena_free(pkg_path);
            continue;
        }

        char *elm_json_path = find_package_elm_json(pkg_path);
        arena_free(pkg_path);
        if (!elm_json_path) {
            continue;
        }

        int exposed_count = 0;
        char **exposed = elm_parse_exposed_modules(elm_json_path, &exposed_count);
        arena_free(elm_json_path);

        if (!exposed || exposed_count <= 0) {
            if (exposed) arena_free(exposed);
            continue;
        }

        char *pkg_display = format_author_name(author, name);
        if (!pkg_display) {
            for (int j = 0; j < exposed_count; j++) {
                arena_free(exposed[j]);
            }
            arena_free(exposed);
            continue;
        }

        for (int j = 0; j < exposed_count; j++) {
            if (exposed[j]) {
                external_module_owner_map_add(out_map, exposed[j], pkg_display);
                arena_free(exposed[j]);
            }
        }
        arena_free(exposed);
    }

    external_module_owner_map_sort(out_map);
    cache_config_free(cache);
    return true;
}

static bool path_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool path_exists_stat(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char *get_realpath_safe(const char *path) {
    char resolved[MAX_PATH_LENGTH];
    if (!realpath(path, resolved)) {
        return NULL;
    }
    return arena_strdup(resolved);
}

static char *compute_relative_path(const char *base_abs, const char *file_abs) {
    /* Both must be absolute paths */
    size_t base_len = strlen(base_abs);
    size_t file_len = strlen(file_abs);

    /* file_abs must start with base_abs + "/" */
    if (file_len <= base_len || strncmp(file_abs, base_abs, base_len) != 0) {
        return NULL;
    }

    /* Skip past base_abs and the separator */
    const char *rel = file_abs + base_len;
    while (*rel == '/') {
        rel++;
    }

    return arena_strdup(rel);
}

static char *compute_dest_relative_from_app_srcdirs(const char *file_abs, char **srcdirs_abs, int srcdir_count) {
    if (!file_abs || !srcdirs_abs || srcdir_count <= 0) return NULL;

    const char *best_base = NULL;
    size_t best_len = 0;

    for (int i = 0; i < srcdir_count; i++) {
        const char *base = srcdirs_abs[i];
        if (!base) continue;

        size_t base_len = strlen(base);
        if (base_len == 0) continue;
        if (strncmp(file_abs, base, base_len) != 0) continue;

        /* Ensure we match a full directory prefix (avoid /src matching /src2). */
        if (file_abs[base_len] != '/') continue;

        if (base_len > best_len) {
            best_base = base;
            best_len = base_len;
        }
    }

    if (!best_base) return NULL;
    return compute_relative_path(best_base, file_abs);
}

/* Forward declarations (defined later in this file) */
static char **get_app_source_dirs_abs(const char *elm_json_path, const char *app_root_dir, int *out_count);
static char *resolve_local_import_to_file(const char *module_name,
                                          char **srcdirs_abs, int srcdir_count);

static const char *find_app_package_version_any(const ElmJson *app_json, const char *author, const char *name) {
    if (!app_json || app_json->type != ELM_PROJECT_APPLICATION || !author || !name) return NULL;

    Package *pkg = NULL;

    if (app_json->dependencies_direct) {
        pkg = package_map_find(app_json->dependencies_direct, author, name);
        if (pkg) return pkg->version;
    }
    if (app_json->dependencies_indirect) {
        pkg = package_map_find(app_json->dependencies_indirect, author, name);
        if (pkg) return pkg->version;
    }
    if (app_json->dependencies_test_direct) {
        pkg = package_map_find(app_json->dependencies_test_direct, author, name);
        if (pkg) return pkg->version;
    }
    if (app_json->dependencies_test_indirect) {
        pkg = package_map_find(app_json->dependencies_test_indirect, author, name);
        if (pkg) return pkg->version;
    }

    return NULL;
}

typedef struct {
    int count;
    int capacity;
    char **items; /* "author/name" */
} StringSet;

static void string_set_init(StringSet *set) {
    if (!set) return;
    set->count = 0;
    set->capacity = INITIAL_MEDIUM_CAPACITY;
    set->items = arena_malloc(set->capacity * sizeof(char*));
}

static bool string_set_contains(const StringSet *set, const char *s) {
    if (!set || !set->items || !s) return false;
    for (int i = 0; i < set->count; i++) {
        if (set->items[i] && strcmp(set->items[i], s) == 0) return true;
    }
    return false;
}

static void string_set_add(StringSet *set, const char *s) {
    if (!set || !s) return;
    if (string_set_contains(set, s)) return;
    DYNARRAY_ENSURE_CAPACITY(set->items, set->count, set->capacity, char*);
    set->items[set->count++] = arena_strdup(s);
}

static PackageMap *compute_extracted_package_dependencies(
    const char *target_src_abs,
    const SelectedFiles *selected,
    const ElmJson *app_json,
    const ExternalModuleOwnerMap *external_map
) {
    if (!target_src_abs || !selected || !app_json || !external_map) return NULL;

    PackageMap *deps = package_map_create();
    if (!deps) return NULL;

    char *srcdirs_local[1];
    srcdirs_local[0] = (char *)target_src_abs;
    int srcdir_local_count = 1;

    for (int i = 0; i < selected->count; i++) {
        if (!path_is_elm_file(selected->abs_paths[i])) {
            continue;
        }

        char dest_file[MAX_PATH_LENGTH];
        int dest_len = snprintf(dest_file, sizeof(dest_file), "%s/%s",
                                target_src_abs, selected->dest_relatives[i]);
        if (dest_len < 0 || dest_len >= (int)sizeof(dest_file)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(dest_file);
        if (!module) {
            continue;
        }

        for (int j = 0; j < module->imports_count; j++) {
            const char *import_name = module->imports[j].module_name;
            if (!import_name) continue;

            /* If it resolves within the new package src/, it is internal. */
            char *resolved_local = resolve_local_import_to_file(import_name, srcdirs_local, srcdir_local_count);
            if (resolved_local) {
                arena_free(resolved_local);
                continue;
            }

            const char *owner_pkg = external_module_owner_map_find(external_map, import_name);
            if (!owner_pkg) {
                continue;
            }

            char *dep_author = NULL;
            char *dep_name = NULL;
            if (!parse_package_name_silent(owner_pkg, &dep_author, &dep_name)) {
                if (dep_author) arena_free(dep_author);
                if (dep_name) arena_free(dep_name);
                continue;
            }

            const char *ver = find_app_package_version_any(app_json, dep_author, dep_name);
            if (ver) {
                (void)package_map_add(deps, dep_author, dep_name, ver);
            }

            arena_free(dep_author);
            arena_free(dep_name);
        }

        skeleton_free(module);
    }

    return deps;
}

static bool demote_unused_app_direct_dependencies(
    const char *app_elm_json_path,
    const char *app_root_dir,
    const char *keep_author,
    const char *keep_name
) {
    if (!app_elm_json_path) return false;

    ElmJson *app_json = elm_json_read(app_elm_json_path);
    if (!app_json || app_json->type != ELM_PROJECT_APPLICATION) {
        if (app_json) elm_json_free(app_json);
        return false;
    }

    int srcdir_count = 0;
    char **srcdirs_abs = get_app_source_dirs_abs(app_elm_json_path, app_root_dir, &srcdir_count);
    if (!srcdirs_abs || srcdir_count <= 0) {
        elm_json_free(app_json);
        return false;
    }

    ExternalModuleOwnerMap external_map = {0};
    bool have_external_map = build_external_module_owner_map_from_app(app_json, &external_map);
    if (!have_external_map) {
        elm_json_free(app_json);
        return false;
    }

    StringSet used_direct_pkgs;
    string_set_init(&used_direct_pkgs);
    if (!used_direct_pkgs.items) {
        elm_json_free(app_json);
        return false;
    }

    int files_capacity = INITIAL_LARGE_CAPACITY;
    int files_count = 0;
    char **files = arena_malloc(files_capacity * sizeof(char*));
    if (!files) {
        elm_json_free(app_json);
        return false;
    }

    for (int i = 0; i < srcdir_count; i++) {
        if (!srcdirs_abs[i]) continue;
        elm_collect_elm_files(srcdirs_abs[i], &files, &files_count, &files_capacity);
    }

    for (int i = 0; i < files_count; i++) {
        if (!files[i]) continue;

        SkeletonModule *module = skeleton_parse(files[i]);
        if (!module) continue;

        for (int j = 0; j < module->imports_count; j++) {
            const char *import_name = module->imports[j].module_name;
            if (!import_name) continue;

            char *resolved_local = resolve_local_import_to_file(import_name, srcdirs_abs, srcdir_count);
            if (resolved_local) {
                arena_free(resolved_local);
                continue;
            }

            const char *owner_pkg = external_module_owner_map_find(&external_map, import_name);
            if (!owner_pkg) continue;
            string_set_add(&used_direct_pkgs, owner_pkg);
        }

        skeleton_free(module);
    }

    int removed_count = 0;
    if (app_json->dependencies_direct) {
        int i = 0;
        while (i < app_json->dependencies_direct->count) {
            Package *pkg = &app_json->dependencies_direct->packages[i];
            if (!pkg->author || !pkg->name) {
                i++;
                continue;
            }

            if (strcmp(pkg->author, "elm") == 0 && strcmp(pkg->name, "core") == 0) {
                i++;
                continue;
            }

            if (keep_author && keep_name &&
                strcmp(pkg->author, keep_author) == 0 && strcmp(pkg->name, keep_name) == 0) {
                i++;
                continue;
            }

            char key[MAX_KEY_LENGTH];
            int n = snprintf(key, sizeof(key), "%s/%s", pkg->author, pkg->name);
            if (n < 0 || n >= (int)sizeof(key)) {
                i++;
                continue;
            }

            if (!string_set_contains(&used_direct_pkgs, key)) {
                if (package_map_remove(app_json->dependencies_direct, pkg->author, pkg->name)) {
                    removed_count++;
                    continue; /* swapped */
                }
            }

            i++;
        }
    }

    bool ok = true;
    if (removed_count > 0) {
        ok = elm_json_write(app_json, app_elm_json_path);
    }

    elm_json_free(app_json);
    return ok;
}

/* ============================================================================
 * File selection
 * ========================================================================== */

static void selected_files_init(SelectedFiles *sf) {
    sf->capacity = INITIAL_FILE_CAPACITY;
    sf->count = 0;
    sf->abs_paths = arena_malloc(sf->capacity * sizeof(char*));
    sf->dest_relatives = arena_malloc(sf->capacity * sizeof(char*));
    sf->source_indices = arena_malloc(sf->capacity * sizeof(int));
}

static void selected_files_add(SelectedFiles *sf, const char *abs_path,
                               const char *dest_relative, int source_index) {
    if (sf->count >= sf->capacity) {
        sf->capacity *= 2;
        sf->abs_paths = arena_realloc(sf->abs_paths, sf->capacity * sizeof(char*));
        sf->dest_relatives = arena_realloc(sf->dest_relatives, sf->capacity * sizeof(char*));
        sf->source_indices = arena_realloc(sf->source_indices, sf->capacity * sizeof(int));
    }
    sf->abs_paths[sf->count] = arena_strdup(abs_path);
    sf->dest_relatives[sf->count] = arena_strdup(dest_relative);
    sf->source_indices[sf->count] = source_index;
    sf->count++;
}

static bool selected_files_contains(const SelectedFiles *sf, const char *abs_path) {
    for (int i = 0; i < sf->count; i++) {
        if (strcmp(sf->abs_paths[i], abs_path) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Collect files recursively from a directory.
 *
 * @param dir_abs        Absolute path of current directory being scanned
 * @param root_abs       Absolute path of the top-level source directory
 * @param dir_basename   Basename of the source directory (for destination paths)
 * @param out            Output: files are added here
 */
static void collect_files_recursive(const char *dir_abs, const char *root_abs,
                                    const char *dir_basename, int source_index,
                                    char **srcdirs_abs, int srcdir_count,
                                    SelectedFiles *out) {
    DIR *dir = opendir(dir_abs);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[MAX_PATH_LENGTH];
        int len = snprintf(child_path, sizeof(child_path), "%s/%s", dir_abs, entry->d_name);
        if (len < 0 || len >= (int)sizeof(child_path)) {
            continue;
        }

        struct stat st;
        if (stat(child_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_files_recursive(child_path, root_abs, dir_basename, source_index,
                                    srcdirs_abs, srcdir_count, out);
        } else if (S_ISREG(st.st_mode)) {
            char *abs = get_realpath_safe(child_path);
            if (abs) {
                /* Prefer preserving the original path within the app's source-directories. */
                char *dest_rel_from_srcdir = compute_dest_relative_from_app_srcdirs(abs, srcdirs_abs, srcdir_count);
                if (dest_rel_from_srcdir) {
                    selected_files_add(out, abs, dest_rel_from_srcdir, source_index);
                    arena_free(dest_rel_from_srcdir);
                } else {
                    /* Fallback: dir_basename + "/" + relative_from_root */
                    char *rel_from_root = compute_relative_path(root_abs, abs);
                    if (rel_from_root) {
                        char dest_rel[MAX_PATH_LENGTH];
                        int dest_len = snprintf(dest_rel, sizeof(dest_rel), "%s/%s",
                                               dir_basename, rel_from_root);
                        arena_free(rel_from_root);

                        if (dest_len >= 0 && dest_len < (int)sizeof(dest_rel)) {
                            selected_files_add(out, abs, dest_rel, source_index);
                        }
                    }
                }
                arena_free(abs);
            }
        }
    }

    closedir(dir);
}

/* ============================================================================
 * Violation tracking
 * ========================================================================== */

static void violation_list_init(ViolationList *vl) {
    vl->capacity = INITIAL_SMALL_CAPACITY;
    vl->count = 0;
    vl->violations = arena_malloc(vl->capacity * sizeof(ExtractViolation));
}

static void violation_list_add(ViolationList *vl, const char *importing_file,
                                const char *importing_mod, const char *imported_mod,
                                const char *imported_file) {
    if (vl->count >= vl->capacity) {
        vl->capacity *= 2;
        vl->violations = arena_realloc(vl->violations,
                                       vl->capacity * sizeof(ExtractViolation));
    }

    ExtractViolation *v = &vl->violations[vl->count++];
    v->importing_file_abs = arena_strdup(importing_file);
    v->importing_module_name = arena_strdup(importing_mod);
    v->imported_module_name = arena_strdup(imported_mod);
    v->imported_file_abs = arena_strdup(imported_file);
}

/* ============================================================================
 * Source directory resolution
 * ========================================================================== */

static char **get_app_source_dirs_abs(const char *elm_json_path, const char *app_root_dir, int *out_count) {
    char **source_dirs = elm_parse_source_directories(elm_json_path, out_count);
    if (!source_dirs || *out_count == 0) {
        /* Default to "src" if not specified */
        *out_count = 1;
        source_dirs = arena_malloc(sizeof(char*));
        source_dirs[0] = arena_strdup("src");
    }

    const char *root = (app_root_dir && app_root_dir[0] != '\0') ? app_root_dir : ".";

    /* Normalize each to absolute path */
    for (int i = 0; i < *out_count; i++) {
        char full_path[MAX_PATH_LENGTH];
        int len = snprintf(full_path, sizeof(full_path), "%s/%s", root, source_dirs[i]);
        if (len < 0 || len >= (int)sizeof(full_path)) {
            return NULL;
        }

        char *abs = get_realpath_safe(full_path);
        if (abs) {
            arena_free(source_dirs[i]);
            source_dirs[i] = abs;
        } else {
            /* Fallback: keep an absolute-ish path even if realpath fails (e.g., missing dir).
             * This prevents accidental resolution relative to the current working directory. */
            arena_free(source_dirs[i]);
            source_dirs[i] = arena_strdup(full_path);
        }
    }

    return source_dirs;
}

/* ============================================================================
 * Import resolution
 * ========================================================================== */

static char *resolve_local_import_to_file(const char *module_name,
                                          char **srcdirs_abs, int srcdir_count) {
    for (int i = 0; i < srcdir_count; i++) {
        char *candidate = elm_module_name_to_path(module_name, srcdirs_abs[i]);
        if (file_exists(candidate)) {
            char *abs = get_realpath_safe(candidate);
            arena_free(candidate);
            return abs;
        }
        arena_free(candidate);
    }
    return NULL;
}

/* ============================================================================
 * File moving
 * ========================================================================== */

static bool move_file(const char *src, const char *dest) {
    /* Create parent directories by iterating through path components */
    char *dest_copy = arena_strdup(dest);
    if (!dest_copy) {
        log_error("Out of memory");
        return false;
    }

    /* Get the parent directory */
    char *last_slash = strrchr(dest_copy, '/');
    if (last_slash) {
        *last_slash = '\0';  /* Temporarily null-terminate to get parent */

        /* Create all parent directories */
        char temp_path[MAX_PATH_LENGTH];
        temp_path[0] = '\0';

        char *component = dest_copy;
        if (dest_copy[0] == '/') {
            strcpy(temp_path, "/");
            component = dest_copy + 1;
        }

        char *slash;
        while ((slash = strchr(component, '/')) != NULL) {
            *slash = '\0';

            if (temp_path[0] == '/' && temp_path[1] == '\0') {
                snprintf(temp_path, sizeof(temp_path), "/%s", component);
            } else if (temp_path[0] != '\0') {
                size_t len = strlen(temp_path);
                snprintf(temp_path + len, sizeof(temp_path) - len, "/%s", component);
            } else {
                snprintf(temp_path, sizeof(temp_path), "%s", component);
            }

            struct stat st;
            if (stat(temp_path, &st) != 0) {
                if (mkdir(temp_path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
                    log_error("Failed to create directory %s: %s", temp_path, strerror(errno));
                    arena_free(dest_copy);
                    return false;
                }
            }

            *slash = '/';
            component = slash + 1;
        }

        /* Create the final parent component */
        if (*component) {
            if (temp_path[0] == '/' && temp_path[1] == '\0') {
                snprintf(temp_path, sizeof(temp_path), "/%s", component);
            } else if (temp_path[0] != '\0') {
                size_t len = strlen(temp_path);
                snprintf(temp_path + len, sizeof(temp_path) - len, "/%s", component);
            } else {
                snprintf(temp_path, sizeof(temp_path), "%s", component);
            }

            struct stat st;
            if (stat(temp_path, &st) != 0) {
                if (mkdir(temp_path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
                    log_error("Failed to create directory %s: %s", temp_path, strerror(errno));
                    arena_free(dest_copy);
                    return false;
                }
            }
        }
    }
    arena_free(dest_copy);

    /* Try rename first */
    if (rename(src, dest) == 0) {
        return true;
    }

    /* If EXDEV (cross-device), fallback to copy + unlink */
    if (errno == EXDEV) {
        size_t size = 0;
        char *content = file_read_contents_bounded(src, MAX_ELM_SOURCE_FILE_BYTES, &size);
        if (!content) {
            log_error("Failed to read source file %s for copying", src);
            return false;
        }

        FILE *out = fopen(dest, "wb");
        if (!out) {
            log_error("Failed to open destination file %s for writing: %s",
                     dest, strerror(errno));
            arena_free(content);
            return false;
        }

        size_t written = fwrite(content, 1, size, out);
        fclose(out);
        arena_free(content);

        if (written != size) {
            log_error("Failed to write complete file to %s", dest);
            return false;
        }

        if (unlink(src) != 0) {
            log_error("Failed to remove source file %s after copy: %s",
                     src, strerror(errno));
            return false;
        }

        return true;
    }

    log_error("Failed to rename %s to %s: %s", src, dest, strerror(errno));
    return false;
}

/* ============================================================================
 * Plan output helpers
 * ========================================================================== */

/* Tree drawing characters (UTF-8) */
#define TREE_BRANCH "├── "
#define TREE_LAST   "└── "
#define TREE_VERT   "│   "
#define TREE_SPACE  "    "

typedef struct FileTreeNode FileTreeNode;
struct FileTreeNode {
    char *name;
    FileTreeNode **children;
    int count;
    int capacity;
    bool is_dir;
};

static FileTreeNode *file_tree_node_create(const char *name, bool is_dir) {
    FileTreeNode *n = arena_calloc(1, sizeof(FileTreeNode));
    if (!n) {
        return NULL;
    }
    n->name = name ? arena_strdup(name) : arena_strdup("");
    n->is_dir = is_dir;
    n->capacity = INITIAL_SMALL_CAPACITY;
    n->children = arena_malloc(n->capacity * sizeof(FileTreeNode*));
    return n;
}

static FileTreeNode *file_tree_find_child(FileTreeNode *parent, const char *name) {
    for (int i = 0; i < parent->count; i++) {
        if (strcmp(parent->children[i]->name, name) == 0) {
            return parent->children[i];
        }
    }
    return NULL;
}

static FileTreeNode *file_tree_add_child(FileTreeNode *parent, const char *name, bool is_dir) {
    FileTreeNode *existing = file_tree_find_child(parent, name);
    if (existing) {
        if (is_dir) {
            existing->is_dir = true;
        }
        return existing;
    }

    if (parent->count >= parent->capacity) {
        parent->capacity *= 2;
        parent->children = arena_realloc(parent->children, parent->capacity * sizeof(FileTreeNode*));
    }

    FileTreeNode *child = file_tree_node_create(name, is_dir);
    if (!child) {
        return NULL;
    }
    parent->children[parent->count++] = child;
    return child;
}

static int file_tree_child_cmp(const void *a, const void *b) {
    const FileTreeNode *na = *(const FileTreeNode * const *)a;
    const FileTreeNode *nb = *(const FileTreeNode * const *)b;
    if (na->is_dir != nb->is_dir) {
        return na->is_dir ? -1 : 1;
    }
    return strcmp(na->name, nb->name);
}

static void file_tree_sort_recursive(FileTreeNode *node) {
    if (!node || node->count <= 1) {
        /* still recurse for 0/1 to ensure deeper children are sorted */
    }
    if (node && node->count > 1) {
        qsort(node->children, (size_t)node->count, sizeof(FileTreeNode*), file_tree_child_cmp);
    }
    if (!node) {
        return;
    }
    for (int i = 0; i < node->count; i++) {
        file_tree_sort_recursive(node->children[i]);
    }
}

static void file_tree_insert_path(FileTreeNode *root, const char *path) {
    if (!root || !path || path[0] == '\0') {
        return;
    }

    char *copy = arena_strdup(path);
    if (!copy) {
        return;
    }

    FileTreeNode *cur = root;
    char *seg = copy;
    while (*seg == '/') {
        seg++;
    }

    while (*seg) {
        char *slash = strchr(seg, '/');
        bool is_last = (slash == NULL);
        if (slash) {
            *slash = '\0';
        }

        if (seg[0] != '\0') {
            cur = file_tree_add_child(cur, seg, !is_last);
            if (!cur) {
                break;
            }
        }

        if (!slash) {
            break;
        }
        seg = slash + 1;
        while (*seg == '/') {
            seg++;
        }
    }

    arena_free(copy);
}

static void file_tree_print_recursive(const FileTreeNode *node, const char *prefix) {
    if (!node) {
        return;
    }

    for (int i = 0; i < node->count; i++) {
        const FileTreeNode *child = node->children[i];
        bool is_last = (i == node->count - 1);

        printf("%s%s%s\n", prefix, is_last ? TREE_LAST : TREE_BRANCH, child->name);

        if (child->is_dir && child->count > 0) {
            size_t prefix_len = strlen(prefix);
            const char *suffix = is_last ? TREE_SPACE : TREE_VERT;
            size_t suffix_len = strlen(suffix);
            char *child_prefix = arena_malloc(prefix_len + suffix_len + 1);
            if (!child_prefix) {
                continue;
            }
            memcpy(child_prefix, prefix, prefix_len);
            memcpy(child_prefix + prefix_len, suffix, suffix_len + 1);
            file_tree_print_recursive(child, child_prefix);
        }
    }
}

static char *get_dirname_alloc(const char *path) {
    if (!path) {
        return NULL;
    }

    char *copy = arena_strdup(path);
    if (!copy) {
        return NULL;
    }

    char *dir = dirname(copy);
    char *out = dir ? arena_strdup(dir) : NULL;
    arena_free(copy);
    return out;
}

static char *compute_app_relative_or_abs(const char *app_root_abs, const char *file_abs) {
    if (app_root_abs && file_abs) {
        char *rel = compute_relative_path(app_root_abs, file_abs);
        if (rel) {
            return rel;
        }
    }
    return file_abs ? arena_strdup(file_abs) : NULL;
}

static char *read_license_from_elm_json(const char *elm_json_path) {
    if (!elm_json_path) {
        return NULL;
    }

    size_t size = 0;
    char *content = file_read_contents_bounded(elm_json_path, MAX_ELM_JSON_FILE_BYTES, &size);
    if (!content) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(content);
    arena_free(content);
    if (!root) {
        return NULL;
    }

    cJSON *license_item = cJSON_GetObjectItem(root, "license");
    char *license = NULL;
    if (license_item && cJSON_IsString(license_item) && license_item->valuestring) {
        license = arena_strdup(license_item->valuestring);
    }

    cJSON_Delete(root);
    return license;
}

static char *read_package_init_template_license(void) {
    if (!embedded_archive_available()) {
        return NULL;
    }

    void *data = NULL;
    size_t size = 0;
    if (!embedded_archive_extract("templates/package/init/elm.json", &data, &size)) {
        return NULL;
    }

    char *json = arena_malloc(size + 1);
    if (!json) {
        arena_free(data);
        return NULL;
    }
    memcpy(json, data, size);
    json[size] = '\0';
    arena_free(data);

    cJSON *root = cJSON_Parse(json);
    arena_free(json);
    if (!root) {
        return NULL;
    }

    cJSON *license_item = cJSON_GetObjectItem(root, "license");
    char *license = NULL;
    if (license_item && cJSON_IsString(license_item) && license_item->valuestring) {
        license = arena_strdup(license_item->valuestring);
    }

    cJSON_Delete(root);
    return license;
}

static const char *version_as_constraint_for_display(const char *version) {
    if (!version) return "(unknown)";
    if (strstr(version, "<= v <") != NULL) {
        return version;
    }
    char *constraint = version_to_constraint(version);
    return constraint ? constraint : version;
}

typedef struct {
    char *module_name;
    char *file_abs;
} ModuleFilePair;

static __attribute__((unused)) ModuleFilePair *build_module_file_pairs(const SelectedFiles *selected, int *out_count) {
    int capacity = INITIAL_MODULE_CAPACITY;
    int count = 0;
    ModuleFilePair *pairs = arena_malloc(capacity * sizeof(ModuleFilePair));
    if (!pairs) {
        *out_count = 0;
        return NULL;
    }

    for (int i = 0; i < selected->count; i++) {
        const char *file_abs = selected->abs_paths[i];
        if (!path_is_elm_file(file_abs)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(file_abs);
        if (!module || !module->module_name) {
            if (module) {
                skeleton_free(module);
            }
            continue;
        }

        DYNARRAY_ENSURE_CAPACITY(pairs, count, capacity, ModuleFilePair);
        pairs[count].module_name = arena_strdup(module->module_name);
        pairs[count].file_abs = arena_strdup(file_abs);
        count++;

        skeleton_free(module);
    }

    *out_count = count;
    return pairs;
}

static __attribute__((unused)) const char *find_file_for_module(const ModuleFilePair *pairs, int count, const char *module_name) {
    if (!pairs || !module_name) {
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        if (pairs[i].module_name && strcmp(pairs[i].module_name, module_name) == 0) {
            return pairs[i].file_abs;
        }
    }

    return NULL;
}

static __attribute__((unused)) void collect_and_print_import_tree_recursive(
    const char *file_abs,
    const SelectedFiles *selected,
    char **srcdirs_abs,
    int srcdir_count,
    const ExternalModuleOwnerMap *external_map,
    char ***visited,
    int *visited_count,
    int *visited_capacity,
    const char *prefix
) {
    if (!file_abs) {
        return;
    }

    for (int i = 0; i < *visited_count; i++) {
        if (strcmp((*visited)[i], file_abs) == 0) {
            return;
        }
    }

    DYNARRAY_PUSH(*visited, *visited_count, *visited_capacity, arena_strdup(file_abs), char*);

    SkeletonModule *mod = skeleton_parse(file_abs);
    if (!mod) {
        return;
    }

    int local_cap = INITIAL_SMALL_CAPACITY;
    int local_count = 0;
    char **local_names = arena_malloc(local_cap * sizeof(char*));
    char **local_paths = arena_malloc(local_cap * sizeof(char*));

    int external_cap = INITIAL_SMALL_CAPACITY;
    int external_count = 0;
    char **external_names = arena_malloc(external_cap * sizeof(char*));

    for (int i = 0; i < mod->imports_count; i++) {
        const char *import_name = mod->imports[i].module_name;
        if (!import_name) {
            continue;
        }

        char *resolved = resolve_local_import_to_file(import_name, srcdirs_abs, srcdir_count);
        if (resolved) {
            if (selected_files_contains(selected, resolved)) {
                DYNARRAY_ENSURE_CAPACITY(local_names, local_count, local_cap, char*);
                DYNARRAY_ENSURE_CAPACITY(local_paths, local_count, local_cap, char*);
                local_names[local_count] = arena_strdup(import_name);
                local_paths[local_count] = arena_strdup(resolved);
                local_count++;
            } else {
                DYNARRAY_PUSH(external_names, external_count, external_cap, arena_strdup(import_name), char*);
            }
            arena_free(resolved);
        } else {
            DYNARRAY_PUSH(external_names, external_count, external_cap, arena_strdup(import_name), char*);
        }
    }

    skeleton_free(mod);

    int total = local_count + external_count;
    int idx = 0;

    for (int i = 0; i < local_count; i++) {
        const char *name = local_names[i];
        const char *path = local_paths[i];

        int already_visited = 0;
        for (int j = 0; j < *visited_count; j++) {
            if (strcmp((*visited)[j], path) == 0) {
                already_visited = 1;
                break;
            }
        }

        int is_last = (idx == total - 1);
        idx++;

        printf("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);

        if (already_visited) {
            printf("%s (already shown)\n", name);
            continue;
        }

        printf("%s\n", name);

        size_t prefix_len = strlen(prefix);
        const char *suffix = is_last ? TREE_SPACE : TREE_VERT;
        size_t suffix_len = strlen(suffix);
        char *child_prefix = arena_malloc(prefix_len + suffix_len + 1);
        if (!child_prefix) {
            continue;
        }
        memcpy(child_prefix, prefix, prefix_len);
        memcpy(child_prefix + prefix_len, suffix, suffix_len + 1);

        collect_and_print_import_tree_recursive(path, selected, srcdirs_abs, srcdir_count,
                            external_map,
                                                visited, visited_count, visited_capacity,
                                                child_prefix);
    }

    for (int i = 0; i < external_count; i++) {
        const char *name = external_names[i];

        int is_last = (idx == total - 1);
        idx++;

        printf("%s%s", prefix, is_last ? TREE_LAST : TREE_BRANCH);
        const char *owner = external_map ? external_module_owner_map_find(external_map, name) : NULL;
        if (owner) {
            printf("%s (%s)\n", name, owner);
        } else {
            printf("%s (external)\n", name);
        }
    }
}



static char **compute_exposed_modules_from_source(const SelectedFiles *selected, int *out_count) {
    int capacity = INITIAL_MODULE_CAPACITY;
    int count = 0;
    char **modules = arena_malloc(capacity * sizeof(char*));
    if (!modules) {
        *out_count = 0;
        return NULL;
    }

    for (int i = 0; i < selected->count; i++) {
        const char *file_abs = selected->abs_paths[i];
        if (!path_is_elm_file(file_abs)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(file_abs);
        if (!module || !module->module_name) {
            if (module) {
                skeleton_free(module);
            }
            continue;
        }

        bool has_exposing = module->exports.expose_all ||
                           module->exports.values_count > 0 ||
                           module->exports.types_count > 0 ||
                           module->exports.types_with_constructors_count > 0;

        if (has_exposing) {
            bool duplicate = false;
            for (int j = 0; j < count; j++) {
                if (strcmp(modules[j], module->module_name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                DYNARRAY_PUSH(modules, count, capacity, arena_strdup(module->module_name), char*);
            }
        }

        skeleton_free(module);
    }

    *out_count = count;
    return modules;
}

static bool show_extract_plan_and_confirm(
    const char *package_name,
    const char *version_str,
    const char *license_str,
    const char *target_path,
    const SelectedFiles *selected,
    const char *app_root_dir,
    const char *app_root_abs,
    const ElmJson *app_json,
    char **srcdirs_abs,
    int srcdir_count,
    bool will_register_local_dev,
    bool auto_yes
) {
    (void)version_str;

    printf("Here is my plan:\n");
    printf("  \n");

    const char *app_root_display = (app_root_dir && app_root_dir[0] != '\0')
        ? app_root_dir
        : ((app_root_abs && app_root_abs[0] != '\0') ? app_root_abs : "(unknown)");
    long file_count = (long)selected->count;
    if (file_count == 1) {
        printf("  I will extract this file from %s:\n\n", app_root_display);
    } else {
        printf("  I will extract %ld files from %s:\n\n", file_count, app_root_display);
    }

    for (int i = 0; i < selected->count; i++) {
        char *rel = compute_app_relative_or_abs(app_root_abs, selected->abs_paths[i]);
        if (rel) {
            printf("    %s\n", rel);
            arena_free(rel);
        }
    }

    printf("\n");
    printf("  Create a new package %s as:\n\n", package_name);

    printf("    %s\n", target_path ? target_path : "(unknown)");

    FileTreeNode *root = file_tree_node_create("", true);
    if (root) {
        file_tree_insert_path(root, "elm.json");

        for (int i = 0; i < selected->count; i++) {
            char buf[MAX_PATH_LENGTH];
            int n = snprintf(buf, sizeof(buf), "src/%s", selected->dest_relatives[i]);
            if (n > 0 && n < (int)sizeof(buf)) {
                file_tree_insert_path(root, buf);
            }
        }

        file_tree_sort_recursive(root);
        file_tree_print_recursive(root, "    ");
    }

    printf("\n");

    ExternalModuleOwnerMap external_map = {0};
    bool have_external_map = build_external_module_owner_map_from_app(app_json, &external_map);

    /* Dependencies (as version ranges from the application's elm.json) */
    int deps_count = 0;
    PackageMap *deps_map = NULL;
    printf("    \n");
    if (have_external_map) {
        deps_map = package_map_create();
        if (deps_map) {
            for (int i = 0; i < selected->count; i++) {
                const char *file_abs = selected->abs_paths[i];
                if (!path_is_elm_file(file_abs)) continue;

                SkeletonModule *module = skeleton_parse(file_abs);
                if (!module) continue;

                for (int j = 0; j < module->imports_count; j++) {
                    const char *import_name = module->imports[j].module_name;
                    if (!import_name) continue;

                    char *resolved_local = resolve_local_import_to_file(import_name, srcdirs_abs, srcdir_count);
                    if (resolved_local) {
                        arena_free(resolved_local);
                        continue;
                    }

                    const char *owner_pkg = external_module_owner_map_find(&external_map, import_name);
                    if (!owner_pkg) {
                        continue;
                    }

                    char *dep_author = NULL;
                    char *dep_name = NULL;
                    if (!parse_package_name_silent(owner_pkg, &dep_author, &dep_name)) {
                        if (dep_author) arena_free(dep_author);
                        if (dep_name) arena_free(dep_name);
                        continue;
                    }

                    const char *ver = find_app_package_version_any(app_json, dep_author, dep_name);
                    if (ver) {
                        (void)package_map_add(deps_map, dep_author, dep_name, ver);
                    }

                    arena_free(dep_author);
                    arena_free(dep_name);
                }

                skeleton_free(module);
            }

            if (deps_map->count > 1) {
                qsort(deps_map->packages, (size_t)deps_map->count, sizeof(Package), compare_packages_by_name);
            }

            deps_count = deps_map->count;
        }
    }

    int exposed_count = 0;
    char **exposed = compute_exposed_modules_from_source(selected, &exposed_count);

    printf("  That exposes %s:\n\n",
           (exposed_count == 1) ? "this module" : "these modules");
    if (exposed && exposed_count > 1) {
        qsort(exposed, (size_t)exposed_count, sizeof(char*), compare_string_ptrs);
    }
    if (!exposed || exposed_count == 0) {
        printf("    (no exposed modules detected)\n");
    } else {
        for (int i = 0; i < exposed_count; i++) {
            if (exposed[i]) {
                printf("    %s\n", exposed[i]);
            }
        }
    }

    printf("\n");

    printf("  With the following %s:\n",
           en_plural_s((long)deps_count, "dependency", "dependencies"));
    printf("    \n");

    if (deps_map && deps_map->count > 0) {
        for (int i = 0; i < deps_map->count; i++) {
            Package *p = &deps_map->packages[i];
            if (!p->author || !p->name || !p->version) continue;
            printf("    %s/%s: %s\n", p->author, p->name, version_as_constraint_for_display(p->version));
        }
    } else {
        printf("    (none)\n");
    }

    printf("\n");

    const char *license_to_show = license_str;
    char *template_license = NULL;
    if (!license_to_show) {
        template_license = read_package_init_template_license();
        license_to_show = template_license;
    }
    if (license_to_show) {
        printf("  License: %s\n\n", license_to_show);
    } else {
        printf("  License: (unknown)\n\n");
    }

    if (will_register_local_dev) {
        if (file_count == 1) {
            printf("  Once I extract this file from the application, I will install %s\n", package_name);
        } else {
            printf("  Once I extract the %ld files from the application, I will install %s\n", file_count, package_name);
        }
        printf("  as the application's direct dependency.\n\n");
        printf("  Also, I will register the package for local development.\n\n");
    }

    printf("To use this package in another application, run from the application directory:\n");
    printf("    %s package install %s\n\n", global_context_program_name(), package_name);

    if (!auto_yes) {
        printf("Would you like me to proceed? [Y/n] ");
        fflush(stdout);

        char response[MAX_TEMP_BUFFER_LENGTH];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
            printf("Aborted.\n");
            return false;
        }
    }

    return true;
}

/* ============================================================================
 * Main command implementation
 * ========================================================================== */

int cmd_extract(int argc, char *argv[]) {
    /* Phase A: Parse arguments */
    bool auto_yes = false;
    bool no_local_dev = false;
    const char *package_spec = NULL;
    const char *target_path = NULL;

    int source_capacity = INITIAL_SMALL_CAPACITY;
    int source_path_count = 0;
    const char **source_paths = arena_malloc(source_capacity * sizeof(char*));
    if (!source_paths) {
        log_error("Out of memory");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_extract_usage();
            return 0;
        } else if (strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0) {
            auto_yes = true;
        } else if (strcmp(arg, "--no-local-dev") == 0) {
            no_local_dev = true;
        } else if (arg[0] == '-' && strcmp(arg, "-") != 0) {
            log_error("Unknown option %s", arg);
            print_extract_usage();
            return 1;
        } else if (!package_spec) {
            package_spec = arg;
        } else if (!target_path) {
            target_path = arg;
        } else {
            DYNARRAY_PUSH(source_paths, source_path_count, source_capacity, arg, const char*);
        }
    }

    if (!package_spec || !target_path || source_path_count == 0) {
        log_error("Insufficient arguments");
        print_extract_usage();
        return 1;
    }

    /* Phase B: Validate application project */
    const char *project_hint_path = source_paths[0];
    char *app_elm_json_path = find_elm_json_upwards(project_hint_path);
    const char *app_elm_json_to_use = app_elm_json_path ? app_elm_json_path : "elm.json";

    char *app_root_dir = get_dirname_alloc(app_elm_json_to_use);
    char *app_root_abs = app_root_dir ? get_realpath_safe(app_root_dir) : NULL;

    ElmJson *app_json = elm_json_read(app_elm_json_to_use);
    if (!app_json) {
        log_error("Could not read elm.json");
        log_error("Have you run 'elm init' or 'wrap init'?");
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    if (app_json->type != ELM_PROJECT_APPLICATION) {
        log_error("This command must be run in an Elm application project (elm.json type=\"application\").");
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Phase C: Parse package specification */
    PackageInstallSpec pkg_spec = {0};
    if (!parse_package_install_spec(package_spec, &pkg_spec)) {
        log_error("Invalid package specification: %s", package_spec);
        log_error("Expected format: author/name or author/name@version");
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    char *author = pkg_spec.author;
    char *name = pkg_spec.name;
    Version version = pkg_spec.version;
    bool has_version = pkg_spec.has_version;

    char *package_name = package_name_from_spec(&pkg_spec);
    if (!package_name) {
        log_error("Failed to format package name");
        arena_free(author);
        arena_free(name);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    char *version_str = NULL;
    if (has_version) {
        version_str = version_to_string(&version);
    }

    arena_free(author);
    arena_free(name);

    /* Phase D: Validate paths */
    if (path_exists_stat(target_path)) {
        log_error("Target path already exists: %s", target_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Validate each source path */
    for (int i = 0; i < source_path_count; i++) {
        const char *src_path = source_paths[i];

        if (!path_exists_stat(src_path)) {
            log_error("Path does not exist: %s", src_path);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            if (app_elm_json_path) arena_free(app_elm_json_path);
            return 1;
        }

        bool is_file = !path_is_directory(src_path);
        if (is_file && !path_is_elm_file(src_path)) {
            log_error("PATH must be an .elm file or a directory: %s", src_path);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            if (app_elm_json_path) arena_free(app_elm_json_path);
            return 1;
        }
    }

    /* Phase E: Resolve application source-directories (used for path preservation + validation) */
    int srcdir_count = 0;
    char **srcdirs_abs = get_app_source_dirs_abs(app_elm_json_to_use, app_root_dir, &srcdir_count);
    if (!srcdirs_abs) {
        log_error("Failed to parse source directories from elm.json");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Phase F: Enumerate selected files from all source paths */
    SelectedFiles selected;
    selected_files_init(&selected);

    for (int i = 0; i < source_path_count; i++) {
        const char *src_path = source_paths[i];
        bool is_file = !path_is_directory(src_path);

        if (is_file) {
            /* Single file: preserve path within app source-directories when possible */
            char *abs = get_realpath_safe(src_path);
            if (!abs) {
                log_error("Failed to resolve path: %s", src_path);
                if (version_str) arena_free(version_str);
                elm_json_free(app_json);
                if (app_elm_json_path) arena_free(app_elm_json_path);
                return 1;
            }

            char *dest_rel_from_srcdir = compute_dest_relative_from_app_srcdirs(abs, srcdirs_abs, srcdir_count);
            if (dest_rel_from_srcdir) {
                selected_files_add(&selected, abs, dest_rel_from_srcdir, i);
                arena_free(dest_rel_from_srcdir);
            } else {
                char *abs_copy = arena_strdup(abs);
                char *file_basename = basename(abs_copy);
                selected_files_add(&selected, abs, file_basename, i);
                arena_free(abs_copy);
            }
            arena_free(abs);
        } else {
            /* Directory: files go under dir_basename/relative_path */
            char *source_abs = get_realpath_safe(src_path);
            if (!source_abs) {
                log_error("Failed to resolve path: %s", src_path);
                if (version_str) arena_free(version_str);
                elm_json_free(app_json);
                if (app_elm_json_path) arena_free(app_elm_json_path);
                return 1;
            }

            char *source_abs_copy = arena_strdup(source_abs);
            char *dir_basename = arena_strdup(basename(source_abs_copy));
            arena_free(source_abs_copy);

            collect_files_recursive(source_abs, source_abs, dir_basename, i,
                                    srcdirs_abs, srcdir_count, &selected);
            arena_free(dir_basename);
            arena_free(source_abs);
        }
    }

    /* Count .elm files */
    int elm_file_count = 0;
    for (int i = 0; i < selected.count; i++) {
        if (path_is_elm_file(selected.abs_paths[i])) {
            elm_file_count++;
        }
    }

    if (elm_file_count == 0) {
        log_error("No .elm files found in specified paths");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Phase G: Out-of-selection import validation */
    ViolationList violations;
    violation_list_init(&violations);

    for (int i = 0; i < selected.count; i++) {
        const char *file_path = selected.abs_paths[i];
        if (!path_is_elm_file(file_path)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(file_path);
        if (!module) {
            log_error("Failed to parse Elm module: %s", file_path);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            if (app_elm_json_path) arena_free(app_elm_json_path);
            return 1;
        }

        for (int j = 0; j < module->imports_count; j++) {
            const char *import_name = module->imports[j].module_name;

            char *resolved = resolve_local_import_to_file(import_name,
                                                         srcdirs_abs, srcdir_count);
            if (resolved) {
                /* This is a local project import */
                if (!selected_files_contains(&selected, resolved)) {
                    /* Violation: imports a local module outside selection */
                    violation_list_add(&violations, file_path, module->module_name,
                                      import_name, resolved);
                }
                arena_free(resolved);
            }
        }

        skeleton_free(module);
    }

    if (violations.count > 0) {
        int offenders_capacity = INITIAL_SMALL_CAPACITY;
        int offenders_count = 0;
        const char **offender_files = arena_malloc(offenders_capacity * sizeof(char*));
        const char **offender_modules = arena_malloc(offenders_capacity * sizeof(char*));

        for (int i = 0; i < violations.count; i++) {
            ExtractViolation *v = &violations.violations[i];

            bool seen = false;
            for (int j = 0; j < offenders_count; j++) {
                if (strcmp(offender_files[j], v->importing_file_abs) == 0) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }

            DYNARRAY_ENSURE_CAPACITY(offender_files, offenders_count, offenders_capacity, const char*);
            DYNARRAY_ENSURE_CAPACITY(offender_modules, offenders_count, offenders_capacity, const char*);
            offender_files[offenders_count] = v->importing_file_abs;
            offender_modules[offenders_count] = v->importing_module_name;
            offenders_count++;
        }

        /* Print an accurate header (singular/plural) */
        if (offenders_count == 1) {
            /* Count missing imports for the single offending module */
            int miss_count_single = 0;
            const char *file_abs0 = offender_files[0];
            SkeletonModule *mod0 = file_abs0 ? skeleton_parse(file_abs0) : NULL;
            if (mod0) {
                for (int m = 0; m < mod0->imports_count; m++) {
                    const char *iname = mod0->imports[m].module_name;
                    if (!iname) continue;

                    char *resolved = resolve_local_import_to_file(iname, srcdirs_abs, srcdir_count);
                    if (resolved) {
                        if (!selected_files_contains(&selected, resolved)) {
                            miss_count_single++;
                        }
                        arena_free(resolved);
                    } else {
                        miss_count_single++;
                    }
                }
                skeleton_free(mod0);
            }

            if (miss_count_single == 1) {
                user_message("I cannot extract the requested package because this module imports a project module outside the selected path.\n\n");
            } else {
                user_message("I cannot extract the requested package because this module imports project modules outside the selected path.\n\n");
            }
        } else {
            user_message("I cannot extract the requested package because some extracted modules import project modules outside the selected path.\n\n");
        }

        for (int i = 0; i < offenders_count; i++) {
            const char *mod_name = offender_modules[i];
            const char *file_abs = offender_files[i];

            /* Print module header */
            user_message("  %s imports:\n", mod_name ? mod_name : (file_abs ? file_abs : "(unknown module)") );

            /* Collect missing imports (app-local missing + external) */
            int miss_cap = INITIAL_SMALL_CAPACITY;
            int miss_count = 0;
            char **miss_names = arena_malloc(miss_cap * sizeof(char*));

            SkeletonModule *mod = skeleton_parse(file_abs);
            if (mod) {
                for (int m = 0; m < mod->imports_count; m++) {
                    const char *iname = mod->imports[m].module_name;
                    if (!iname) continue;

                    char *resolved = resolve_local_import_to_file(iname, srcdirs_abs, srcdir_count);
                    if (resolved) {
                        if (!selected_files_contains(&selected, resolved)) {
                            DYNARRAY_ENSURE_CAPACITY(miss_names, miss_count, miss_cap, char*);
                            miss_names[miss_count++] = arena_strdup(iname);
                        }
                        arena_free(resolved);
                    } else {
                        DYNARRAY_ENSURE_CAPACITY(miss_names, miss_count, miss_cap, char*);
                        miss_names[miss_count++] = arena_strdup(iname);
                    }
                }
                skeleton_free(mod);
            }

            /* Print missing imports */
            for (int m = 0; m < miss_count; m++) {
                user_message("    ✗ %s\n", miss_names[m]);
            }

            user_message("\n");
        }

        user_message("Hint: Extract a directory that includes these modules, or refactor your imports.\n");

        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Phase F2: Read license from application's elm.json (if present) */
    char *license_str = read_license_from_elm_json(app_elm_json_to_use);

    /* Phase F3: Show plan and confirm */
    if (!show_extract_plan_and_confirm(package_name, version_str, license_str,
                                       target_path,
                                       &selected, app_root_dir, app_root_abs,
                                       app_json,
                                       srcdirs_abs, srcdir_count,
                                       !no_local_dev, auto_yes)) {
        arena_free(package_name);
        if (version_str) arena_free(version_str);
        if (license_str) arena_free(license_str);
        if (app_root_dir) arena_free(app_root_dir);
        if (app_root_abs) arena_free(app_root_abs);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 0;
    }

    if (!no_local_dev) {
        printf("Saving elm.json...\n\n");
    }

    /* Phase G: Create TARGET_PATH and initialize package */
    errno = 0;
    if (!mkdir_p(target_path)) {
        int mkdir_failed_errno = errno;
        char cwd_for_error[MAX_PATH_LENGTH];
        const char *cwd_str = (getcwd(cwd_for_error, sizeof(cwd_for_error)) != NULL) ? cwd_for_error : "(unknown)";

        log_error("Failed to create directory: %s", target_path);
        if (mkdir_failed_errno != 0) {
            log_error("Reason: %s", strerror(mkdir_failed_errno));
        }
        log_error("Current directory: %s", cwd_str);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Call shared init helper */
    int init_result = package_init_at_path(target_path, package_spec, !no_local_dev, true);
    if (init_result != 0) {
        log_error("Failed to initialize package at %s", target_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Phase H: Move files to TARGET_PATH/src/ */
    char target_src[MAX_PATH_LENGTH];
    int target_src_len = snprintf(target_src, sizeof(target_src), "%s/src", target_path);
    if (target_src_len < 0 || target_src_len >= (int)sizeof(target_src)) {
        log_error("Target path too long");
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    for (int i = 0; i < selected.count; i++) {
        const char *src_file = selected.abs_paths[i];
        const char *dest_relative = selected.dest_relatives[i];

        char dest_file[MAX_PATH_LENGTH];
        int dest_len = snprintf(dest_file, sizeof(dest_file), "%s/%s",
                               target_src, dest_relative);

        if (dest_len < 0 || dest_len >= (int)sizeof(dest_file)) {
            log_error("Destination path too long");
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            if (app_elm_json_path) arena_free(app_elm_json_path);
            return 1;
        }

        if (!move_file(src_file, dest_file)) {
            log_error("Failed to move %s -> %s", src_file, dest_file);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            if (app_elm_json_path) arena_free(app_elm_json_path);
            return 1;
        }
    }

    /* Phase I: Build exposed-modules list */
    char **exposed_modules = arena_malloc(elm_file_count * sizeof(char*));
    int exposed_count = 0;

    for (int i = 0; i < selected.count; i++) {
        if (!path_is_elm_file(selected.abs_paths[i])) {
            continue;
        }

        /* Use precomputed destination path */
        char dest_file[MAX_PATH_LENGTH];
        int dest_len = snprintf(dest_file, sizeof(dest_file), "%s/%s",
                               target_src, selected.dest_relatives[i]);
        if (dest_len < 0 || dest_len >= (int)sizeof(dest_file)) {
            continue;
        }

        SkeletonModule *module = skeleton_parse(dest_file);
        if (!module || !module->module_name) {
            if (module) skeleton_free(module);
            continue;
        }

        /* Check if module has an exposing clause */
        bool has_exposing = module->exports.expose_all ||
                           module->exports.values_count > 0 ||
                           module->exports.types_count > 0 ||
                           module->exports.types_with_constructors_count > 0;

        if (has_exposing) {
            /* Deduplicate */
            bool already_added = false;
            for (int j = 0; j < exposed_count; j++) {
                if (strcmp(exposed_modules[j], module->module_name) == 0) {
                    already_added = true;
                    break;
                }
            }

            if (!already_added) {
                exposed_modules[exposed_count++] = arena_strdup(module->module_name);
            }
        }

        skeleton_free(module);
    }

    /* Phase J: Update package elm.json with exposed-modules */
    char pkg_elm_json_path[MAX_PATH_LENGTH];
    snprintf(pkg_elm_json_path, sizeof(pkg_elm_json_path), "%s/elm.json", target_path);

    char *target_src_abs = get_realpath_safe(target_src);
    if (!target_src_abs) {
        log_error("Failed to resolve absolute path for %s", target_src);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    /* Phase J1: Add package dependencies to the new package elm.json */
    ExternalModuleOwnerMap external_map = {0};
    bool have_external_map = build_external_module_owner_map_from_app(app_json, &external_map);
    PackageMap *pkg_deps = have_external_map
        ? compute_extracted_package_dependencies(target_src_abs, &selected, app_json, &external_map)
        : NULL;

    if (pkg_deps && pkg_deps->count > 0) {
        ElmJson *pkg_json = elm_json_read(pkg_elm_json_path);
        if (!pkg_json || pkg_json->type != ELM_PROJECT_PACKAGE) {
            log_error("Failed to read package elm.json at %s", pkg_elm_json_path);
            if (pkg_json) elm_json_free(pkg_json);
            arena_free(target_src_abs);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }

        for (int i = 0; i < pkg_deps->count; i++) {
            Package *dep = &pkg_deps->packages[i];
            if (!dep->author || !dep->name || !dep->version) continue;
            (void)add_or_update_package_in_elm_json(pkg_json, dep->author, dep->name, dep->version,
                                                    false /* is_test */, true /* is_direct */, false /* remove_first */);
        }

        if (!elm_json_write(pkg_json, pkg_elm_json_path)) {
            log_error("Failed to write updated package dependencies to %s", pkg_elm_json_path);
            elm_json_free(pkg_json);
            arena_free(target_src_abs);
            if (version_str) arena_free(version_str);
            elm_json_free(app_json);
            return 1;
        }

        elm_json_free(pkg_json);
    }

    size_t elm_json_size = 0;
    char *elm_json_content = file_read_contents_bounded(pkg_elm_json_path,
                                                        MAX_ELM_JSON_FILE_BYTES,
                                                        &elm_json_size);
    if (!elm_json_content) {
        log_error("Failed to read %s", pkg_elm_json_path);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    cJSON *root = cJSON_Parse(elm_json_content);
    arena_free(elm_json_content);

    if (!root) {
        log_error("Failed to parse %s", pkg_elm_json_path);
        arena_free(target_src_abs);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    cJSON *exposed_array = cJSON_CreateArray();
    for (int i = 0; i < exposed_count; i++) {
        cJSON_AddItemToArray(exposed_array, cJSON_CreateString(exposed_modules[i]));
    }

    cJSON_ReplaceItemInObject(root, "exposed-modules", exposed_array);

    if (license_str) {
        cJSON *existing_license = cJSON_GetObjectItem(root, "license");
        const char *existing = (existing_license && cJSON_IsString(existing_license) && existing_license->valuestring)
            ? existing_license->valuestring
            : NULL;

        if (!existing || strcmp(existing, license_str) != 0) {
            cJSON_ReplaceItemInObject(root, "license", cJSON_CreateString(license_str));
        }
    }

    if (!elm_json_write_formatted_atomic(root, pkg_elm_json_path)) {
        log_error("Failed to write updated elm.json");
        cJSON_Delete(root);
        arena_free(target_src_abs);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        return 1;
    }

    cJSON_Delete(root);
    arena_free(target_src_abs);

    /* Phase K: Add package as local-dev dependency to application */
    if (no_local_dev) {
        printf("Successfully extracted %d %s to %s.\n",
               selected.count,
               en_plural_s((long)selected.count, "file", "files"),
               target_path);

        arena_free(package_name);
        if (version_str) arena_free(version_str);
        if (license_str) arena_free(license_str);
        if (app_root_dir) arena_free(app_root_dir);
        if (app_root_abs) arena_free(app_root_abs);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 0;
    }

    char *target_abs = get_realpath_safe(target_path);
    if (!target_abs) {
        log_error("Failed to resolve absolute path for %s", target_path);
        if (version_str) arena_free(version_str);
        if (license_str) arena_free(license_str);
        if (app_root_dir) arena_free(app_root_dir);
        if (app_root_abs) arena_free(app_root_abs);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    InstallEnv *env = install_env_create();
    if (!env) {
        log_error("Failed to create install environment");
        arena_free(target_abs);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    if (!install_env_init(env)) {
        log_error("Failed to initialize install environment");
        install_env_free(env);
        arena_free(target_abs);
        if (version_str) arena_free(version_str);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    int install_result = install_local_dev(target_abs, package_name, app_elm_json_to_use,
                                          env, false, true, true);
    install_env_free(env);
    arena_free(target_abs);

    if (install_result != 0) {
        log_error("Package was created and files moved, but failed to add as dependency.");
        log_error("You can manually add it with: %s package install %s",
                 global_context_program_name(), package_name);
        arena_free(package_name);
        if (version_str) arena_free(version_str);
        if (license_str) arena_free(license_str);
        if (app_root_dir) arena_free(app_root_dir);
        if (app_root_abs) arena_free(app_root_abs);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

    /* Phase L: Demote app direct dependencies that are no longer directly imported */
    {
        char *keep_author = NULL;
        char *keep_name = NULL;
        if (parse_package_name_silent(package_name, &keep_author, &keep_name)) {
            (void)demote_unused_app_direct_dependencies(app_elm_json_to_use, app_root_dir, keep_author, keep_name);
        }
        if (keep_author) arena_free(keep_author);
        if (keep_name) arena_free(keep_name);
    }

    char *pkg_author = NULL;
    char *pkg_name_tmp = NULL;
    char *pkg_version = NULL;
    if (!read_package_info_from_elm_json(pkg_elm_json_path, &pkg_author, &pkg_name_tmp, &pkg_version)) {
        log_error("Failed to read package info from %s", pkg_elm_json_path);
        arena_free(package_name);
        if (version_str) arena_free(version_str);
        if (license_str) arena_free(license_str);
        if (app_root_dir) arena_free(app_root_dir);
        if (app_root_abs) arena_free(app_root_abs);
        elm_json_free(app_json);
        if (app_elm_json_path) arena_free(app_elm_json_path);
        return 1;
    }

        printf("Successfully extracted %d %s to %s and added as local-dev dependency.\n\n",
            selected.count,
            en_plural_s((long)selected.count, "file", "files"),
            target_path);

    printf("Successfully installed %s %s as a direct dependency in %s.\n\n",
           package_name, pkg_version ? pkg_version : "(unknown)", app_elm_json_to_use);

    printf("Please compile the application to confirm it still works.\n");

    if (pkg_author) arena_free(pkg_author);
    if (pkg_name_tmp) arena_free(pkg_name_tmp);
    if (pkg_version) arena_free(pkg_version);

    arena_free(package_name);

    if (version_str) arena_free(version_str);
    if (license_str) arena_free(license_str);
    if (app_root_dir) arena_free(app_root_dir);
    if (app_root_abs) arena_free(app_root_abs);
    elm_json_free(app_json);
    if (app_elm_json_path) arena_free(app_elm_json_path);

    return 0;
}
