#include "package_common.h"
#include "../../alloc.h"
#include "../../cache.h"
#include "../../constants.h"
#include "../../fileutil.h"
#include "../../registry.h"
#include "../../protocol_v2/solver/v2_registry.h"
#include "../../log.h"
#include "../../rulr/rulr.h"
#include "../../rulr/rulr_dl.h"
#include "../../rulr/host_helpers.h"
#include "../../rulr/runtime/runtime.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

bool parse_package_name(const char *package, char **author, char **name) {
    if (!package) return false;

    const char *slash = strchr(package, '/');
    if (!slash) {
        fprintf(stderr, "Error: Package name must be in format 'author/package'\n");
        return false;
    }

    size_t author_len = slash - package;
    *author = arena_malloc(author_len + 1);
    if (!*author) return false;
    strncpy(*author, package, author_len);
    (*author)[author_len] = '\0';

    *name = arena_strdup(slash + 1);
    if (!*name) {
        arena_free(*author);
        return false;
    }

    return true;
}

Package* find_existing_package(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || !author || !name) {
        return NULL;
    }

    Package *pkg = NULL;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        pkg = package_map_find(elm_json->dependencies_direct, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_indirect, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_test_direct, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->dependencies_test_indirect, author, name);
        if (pkg) return pkg;
    } else {
        pkg = package_map_find(elm_json->package_dependencies, author, name);
        if (pkg) return pkg;

        pkg = package_map_find(elm_json->package_test_dependencies, author, name);
        if (pkg) return pkg;
    }

    return NULL;
}

bool read_package_info_from_elm_json(const char *elm_json_path, char **out_author, char **out_name, char **out_version) {
    ElmJson *pkg_elm_json = elm_json_read(elm_json_path);
    if (!pkg_elm_json) {
        return false;
    }

    if (pkg_elm_json->type != ELM_PROJECT_PACKAGE) {
        fprintf(stderr, "Error: The elm.json at %s is not a package project\n", elm_json_path);
        elm_json_free(pkg_elm_json);
        return false;
    }

    if (pkg_elm_json->package_name) {
        if (!parse_package_name(pkg_elm_json->package_name, out_author, out_name)) {
            elm_json_free(pkg_elm_json);
            return false;
        }
    } else {
        fprintf(stderr, "Error: No package name found in elm.json\n");
        elm_json_free(pkg_elm_json);
        return false;
    }

    if (pkg_elm_json->package_version) {
        *out_version = arena_strdup(pkg_elm_json->package_version);
    } else {
        fprintf(stderr, "Error: No version found in elm.json\n");
        elm_json_free(pkg_elm_json);
        return false;
    }

    elm_json_free(pkg_elm_json);
    return true;
}

char* version_to_constraint(const char *version) {
    if (!version) return NULL;

    int major, minor, patch;
    if (sscanf(version, "%d.%d.%d", &major, &minor, &patch) != 3) {
        return NULL;
    }

    /* Format: "X.Y.Z <= v < (X+1).0.0" */
    char *constraint = arena_malloc(MAX_RANGE_STRING_LENGTH);
    if (!constraint) return NULL;

    snprintf(constraint, MAX_RANGE_STRING_LENGTH, "%d.%d.%d <= v < %d.0.0",
             major, minor, patch, major + 1);

    return constraint;
}

static bool ensure_path_exists(const char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }

    char *mutable_path = arena_strdup(path);
    if (!mutable_path) {
        return false;
    }

    bool ok = true;
    size_t len = strlen(mutable_path);
    for (size_t i = 1; i < len && ok; i++) {
        if (mutable_path[i] == '/' || mutable_path[i] == '\\') {
            char saved = mutable_path[i];
            mutable_path[i] = '\0';
            struct stat st;
            if (mutable_path[0] != '\0' && stat(mutable_path, &st) != 0) {
                if (mkdir(mutable_path, DIR_PERMISSIONS) != 0) {
                    ok = false;
                }
            }
            mutable_path[i] = saved;
        }
    }

    if (ok) {
        struct stat st;
        if (stat(mutable_path, &st) != 0) {
            if (mkdir(mutable_path, DIR_PERMISSIONS) != 0) {
                ok = false;
            }
        }
    }

    arena_free(mutable_path);
    return ok;
}

bool install_from_file(const char *source_path, InstallEnv *env, const char *author, const char *name, const char *version) {
    struct stat st;
    if (stat(source_path, &st) != 0) {
        fprintf(stderr, "Error: Path does not exist: %s\n", source_path);
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Source path must be a directory\n");
        return false;
    }

    size_t pkg_base_len = strlen(env->cache->packages_dir) + strlen(author) + strlen(name) + 3;
    char *pkg_base_dir = arena_malloc(pkg_base_len);
    if (!pkg_base_dir) {
        fprintf(stderr, "Error: Failed to allocate package base directory\n");
        return false;
    }
    snprintf(pkg_base_dir, pkg_base_len, "%s/%s/%s", env->cache->packages_dir, author, name);

    char *dest_path = cache_get_package_path(env->cache, author, name, version);
    if (!dest_path) {
        fprintf(stderr, "Error: Failed to get package path\n");
        arena_free(pkg_base_dir);
        return false;
    }

    if (!ensure_path_exists(pkg_base_dir)) {
        fprintf(stderr, "Error: Failed to create package base directory: %s\n", pkg_base_dir);
        arena_free(pkg_base_dir);
        arena_free(dest_path);
        return false;
    }

    if (stat(dest_path, &st) == 0) {
        if (!remove_directory_recursive(dest_path)) {
            fprintf(stderr, "Warning: Failed to remove existing directory: %s\n", dest_path);
        }
    }

    char elm_json_check[PATH_MAX];
    snprintf(elm_json_check, sizeof(elm_json_check), "%s/elm.json", source_path);

    bool result;
    if (stat(elm_json_check, &st) == 0) {
        result = copy_directory_selective(source_path, dest_path);
    } else {
        char *extracted_dir = find_first_subdirectory(source_path);
        if (!extracted_dir) {
            fprintf(stderr, "Error: Could not find package directory in %s\n", source_path);
            arena_free(pkg_base_dir);
            arena_free(dest_path);
            return false;
        }

        result = copy_directory_selective(extracted_dir, dest_path);
        arena_free(extracted_dir);
    }

    if (!result) {
        fprintf(stderr, "Error: Failed to install package to destination\n");
        arena_free(pkg_base_dir);
        arena_free(dest_path);
        return false;
    }

    char src_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "%s/src", dest_path);
    if (stat(src_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: Package installation failed - no src directory found at %s\n", src_path);
        arena_free(pkg_base_dir);
        arena_free(dest_path);
        return false;
    }

    arena_free(pkg_base_dir);
    arena_free(dest_path);
    return true;
}

int compare_package_changes(const void *a, const void *b) {
    const PackageChange *pkg_a = (const PackageChange *)a;
    const PackageChange *pkg_b = (const PackageChange *)b;

    int author_cmp = strcmp(pkg_a->author, pkg_b->author);
    if (author_cmp != 0) return author_cmp;

    return strcmp(pkg_a->name, pkg_b->name);
}

char* find_package_elm_json(const char *pkg_path) {
    size_t direct_len = strlen(pkg_path) + strlen("/elm.json") + 1;
    char *direct_path = arena_malloc(direct_len);
    if (!direct_path) return NULL;
    snprintf(direct_path, direct_len, "%s/elm.json", pkg_path);

    struct stat st;
    if (stat(direct_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return direct_path;
    }
    arena_free(direct_path);

    DIR *dir = opendir(pkg_path);
    if (!dir) return NULL;

    struct dirent *entry;
    char *found_path = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t subdir_len = strlen(pkg_path) + strlen(entry->d_name) + 2;
        char *subdir_path = arena_malloc(subdir_len);
        if (!subdir_path) continue;
        snprintf(subdir_path, subdir_len, "%s/%s", pkg_path, entry->d_name);

        if (stat(subdir_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t elm_json_len = strlen(subdir_path) + strlen("/elm.json") + 1;
            char *elm_json_path = arena_malloc(elm_json_len);
            if (elm_json_path) {
                snprintf(elm_json_path, elm_json_len, "%s/elm.json", subdir_path);
                if (stat(elm_json_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    found_path = elm_json_path;
                    arena_free(subdir_path);
                    break;
                }
                arena_free(elm_json_path);
            }
        }
        arena_free(subdir_path);
    }

    closedir(dir);
    return found_path;
}

bool package_exists_in_registry(InstallEnv *env, const char *author, const char *name,
                                 size_t *out_version_count) {
    size_t version_count = 0;

    if (env->protocol_mode == PROTOCOL_V2) {
        if (!env->v2_registry) {
            log_error("V2 protocol active but registry is not loaded");
            if (out_version_count) *out_version_count = 0;
            return false;
        }

        V2PackageEntry *entry = v2_registry_find(env->v2_registry, author, name);
        if (!entry) {
            if (out_version_count) *out_version_count = 0;
            return false;
        }

        /* Count valid versions */
        for (size_t i = 0; i < entry->version_count; i++) {
            if (entry->versions[i].status == V2_STATUS_VALID) {
                version_count++;
            }
        }

        if (out_version_count) *out_version_count = version_count;
        return (version_count > 0);
    } else {
        RegistryEntry *registry_entry = registry_find(env->registry, author, name);
        if (!registry_entry) {
            if (out_version_count) *out_version_count = 0;
            return false;
        }

        version_count = registry_entry->version_count;
        if (out_version_count) *out_version_count = version_count;
        return true;
    }
}

/**
 * Recursively insert package_dependency facts for a package and all its transitive dependencies.
 * This builds the complete dependency graph needed for orphan detection.
 */
/**
 * Recursively insert package_dependency facts for a package and all its transitive dependencies.
 * This builds the complete dependency graph needed for orphan detection.
 *
 * @param rulr    Rulr engine instance
 * @param cache   Cache config for looking up package paths
 * @param author  Package author
 * @param name    Package name
 * @param version Package version
 * @param visited Package map to track visited packages (prevents cycles)
 */
static void insert_package_dependencies_recursive(
    Rulr *rulr,
    CacheConfig *cache,
    const char *author,
    const char *name,
    const char *version,
    PackageMap *visited
) {
    /* Check if we've already processed this package */
    if (package_map_find(visited, author, name)) {
        return;
    }

    /* Mark as visited */
    package_map_add(visited, author, name, version);

    /* Get the package path in cache */
    char *pkg_path = cache_get_package_path(cache, author, name, version);
    if (!pkg_path) {
        log_debug("Could not get cache path for %s/%s %s", author, name, version);
        return;
    }

    /* Build path to elm.json */
    size_t elm_json_len = strlen(pkg_path) + 12; /* /elm.json\0 */
    char *elm_json_path = arena_malloc(elm_json_len);
    if (!elm_json_path) {
        arena_free(pkg_path);
        return;
    }
    snprintf(elm_json_path, elm_json_len, "%s/elm.json", pkg_path);

    /* Read the package's elm.json */
    ElmJson *pkg_elm_json = elm_json_read(elm_json_path);
    arena_free(elm_json_path);
    arena_free(pkg_path);

    if (!pkg_elm_json) {
        log_debug("Could not read elm.json for %s/%s %s", author, name, version);
        return;
    }

    /* Insert package_dependency facts for this package's dependencies */
    PackageMap *deps = NULL;
    if (pkg_elm_json->type == ELM_PROJECT_PACKAGE) {
        deps = pkg_elm_json->package_dependencies;
    } else {
        /* Applications have both direct and indirect */
        deps = pkg_elm_json->dependencies_direct;
    }

    if (deps) {
        for (int i = 0; i < deps->count; i++) {
            Package *dep = &deps->packages[i];
            rulr_insert_fact_4s(rulr, "package_dependency",
                author, name, dep->author, dep->name);

            /* Recursively process this dependency */
            insert_package_dependencies_recursive(rulr, cache,
                dep->author, dep->name, dep->version, visited);
        }
    }

    /* For applications, also process indirect dependencies */
    if (pkg_elm_json->type == ELM_PROJECT_APPLICATION && pkg_elm_json->dependencies_indirect) {
        for (int i = 0; i < pkg_elm_json->dependencies_indirect->count; i++) {
            Package *dep = &pkg_elm_json->dependencies_indirect->packages[i];
            rulr_insert_fact_4s(rulr, "package_dependency",
                author, name, dep->author, dep->name);

            /* Recursively process this dependency */
            insert_package_dependencies_recursive(rulr, cache,
                dep->author, dep->name, dep->version, visited);
        }
    }

    elm_json_free(pkg_elm_json);
}

bool find_orphaned_packages(
    const ElmJson *elm_json,
    CacheConfig *cache,
    const char *exclude_author,
    const char *exclude_name,
    PackageMap **out_orphaned
) {
    *out_orphaned = NULL;

    if (elm_json->type != ELM_PROJECT_APPLICATION) {
        /* For packages, we don't have the direct/indirect distinction */
        return true;
    }

    log_debug("Finding orphaned dependencies%s%s%s",
        exclude_author ? " (excluding " : "",
        exclude_author ? exclude_author : "",
        exclude_author ? ")" : "");

    /* Initialize rulr */
    Rulr rulr;
    RulrError err = rulr_init(&rulr);
    if (err.is_error) {
        log_error("Failed to initialize rulr: %s", err.message);
        return false;
    }

    /* Load the no_orphaned_packages rule */
    err = rulr_load_rule_file(&rulr, "no_orphaned_packages");
    if (err.is_error) {
        log_error("Failed to load no_orphaned_packages rule: %s", err.message);
        rulr_deinit(&rulr);
        return false;
    }

    /* Insert direct_dependency facts (optionally excluding a target package) */
    if (elm_json->dependencies_direct) {
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            if (exclude_author && exclude_name &&
                strcmp(pkg->author, exclude_author) == 0 &&
                strcmp(pkg->name, exclude_name) == 0) {
                continue; /* Skip the excluded package */
            }
            rulr_insert_fact_2s(&rulr, "direct_dependency", pkg->author, pkg->name);
        }
    }

    if (elm_json->dependencies_test_direct) {
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            if (exclude_author && exclude_name &&
                strcmp(pkg->author, exclude_author) == 0 &&
                strcmp(pkg->name, exclude_name) == 0) {
                continue; /* Skip the excluded package */
            }
            rulr_insert_fact_2s(&rulr, "direct_dependency", pkg->author, pkg->name);
        }
    }

    /* Insert indirect_dependency facts */
    if (elm_json->dependencies_indirect) {
        for (int i = 0; i < elm_json->dependencies_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_indirect->packages[i];
            rulr_insert_fact_2s(&rulr, "indirect_dependency", pkg->author, pkg->name);
        }
    }

    if (elm_json->dependencies_test_indirect) {
        for (int i = 0; i < elm_json->dependencies_test_indirect->count; i++) {
            Package *pkg = &elm_json->dependencies_test_indirect->packages[i];
            rulr_insert_fact_2s(&rulr, "indirect_dependency", pkg->author, pkg->name);
        }
    }

    /* Build the dependency graph by recursively processing all direct dependencies */
    PackageMap *visited = package_map_create();
    if (!visited) {
        log_error("Failed to create visited package map");
        rulr_deinit(&rulr);
        return false;
    }

    if (elm_json->dependencies_direct) {
        for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_direct->packages[i];
            if (exclude_author && exclude_name &&
                strcmp(pkg->author, exclude_author) == 0 &&
                strcmp(pkg->name, exclude_name) == 0) {
                continue; /* Skip the excluded package */
            }
            insert_package_dependencies_recursive(&rulr, cache,
                pkg->author, pkg->name, pkg->version, visited);
        }
    }

    if (elm_json->dependencies_test_direct) {
        for (int i = 0; i < elm_json->dependencies_test_direct->count; i++) {
            Package *pkg = &elm_json->dependencies_test_direct->packages[i];
            if (exclude_author && exclude_name &&
                strcmp(pkg->author, exclude_author) == 0 &&
                strcmp(pkg->name, exclude_name) == 0) {
                continue; /* Skip the excluded package */
            }
            insert_package_dependencies_recursive(&rulr, cache,
                pkg->author, pkg->name, pkg->version, visited);
        }
    }

    package_map_free(visited);

    /* Evaluate the rule */
    err = rulr_evaluate(&rulr);
    if (err.is_error) {
        log_error("Failed to evaluate orphaned packages rule: %s", err.message);
        rulr_deinit(&rulr);
        return false;
    }

    /* Get the orphaned packages */
    EngineRelationView orphaned_view = rulr_get_relation(&rulr, "orphaned");
    if (orphaned_view.pred_id >= 0 && orphaned_view.num_tuples > 0) {
        log_debug("Found %d orphaned package(s)", orphaned_view.num_tuples);

        PackageMap *orphaned = package_map_create();
        if (!orphaned) {
            log_error("Failed to create orphaned package map");
            rulr_deinit(&rulr);
            return false;
        }

        const Tuple *tuples = (const Tuple *)orphaned_view.tuples;
        for (int i = 0; i < orphaned_view.num_tuples; i++) {
            const Tuple *t = &tuples[i];
            if (t->arity != 2 || t->fields[0].kind != VAL_SYM || t->fields[1].kind != VAL_SYM) {
                continue;
            }

            const char *orphan_author = rulr_lookup_symbol(&rulr, t->fields[0].u.sym);
            const char *orphan_name = rulr_lookup_symbol(&rulr, t->fields[1].u.sym);

            if (!orphan_author || !orphan_name) {
                continue;
            }

            log_debug("Orphaned: %s/%s", orphan_author, orphan_name);

            /* Find the version in elm.json */
            Package *pkg = NULL;
            if (elm_json->dependencies_indirect) {
                pkg = package_map_find(elm_json->dependencies_indirect, orphan_author, orphan_name);
            }
            if (!pkg && elm_json->dependencies_test_indirect) {
                pkg = package_map_find(elm_json->dependencies_test_indirect, orphan_author, orphan_name);
            }

            const char *version = pkg ? pkg->version : "0.0.0";
            package_map_add(orphaned, orphan_author, orphan_name, version);
        }

        *out_orphaned = orphaned;
    }

    rulr_deinit(&rulr);
    return true;
}
