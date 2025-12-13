/**
 * install_local_dev.c - Local development package installation
 *
 * Implements the --local-dev flag for `wrap package install` which creates
 * symlinks instead of copying package files, enabling live development.
 */

#include "install_local_dev.h"
#include "package_common.h"
#include "../../local_dev/local_dev_tracking.h"
#include "../../alloc.h"
#include "../../constants.h"
#include "../../log.h"
#include "../../fileutil.h"
#include "../../env_defaults.h"
#include "../../elm_json.h"
#include "../../cache.h"
#include "../../registry.h"
#include "../../solver.h"
#include "../../global_context.h"
#include "../../protocol_v2/solver/v2_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Simple hash function for path -> filename */
static unsigned long hash_path(const char *str) {
    unsigned long hash = DJB2_HASH_INIT;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

char *get_local_dev_tracking_dir(void) {
    char *wrap_home = env_get_wrap_home();
    if (!wrap_home) {
        log_error("WRAP_HOME is not configured");
        return NULL;
    }

    size_t len = strlen(wrap_home) + strlen("/") + strlen(LOCAL_DEV_TRACKING_DIR) + 1;
    char *tracking_dir = arena_malloc(len);
    if (!tracking_dir) {
        arena_free(wrap_home);
        return NULL;
    }
    snprintf(tracking_dir, len, "%s/%s", wrap_home, LOCAL_DEV_TRACKING_DIR);
    arena_free(wrap_home);
    return tracking_dir;
}

/**
 * Create all directories in a path (like mkdir -p).
 */
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
        if (mutable_path[i] == '/') {
            mutable_path[i] = '\0';
            struct stat st;
            if (stat(mutable_path, &st) != 0) {
                if (mkdir(mutable_path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
                    log_error("Failed to create directory: %s", mutable_path);
                    ok = false;
                }
            }
            mutable_path[i] = '/';
        }
    }

    if (ok) {
        struct stat st;
        if (stat(mutable_path, &st) != 0) {
            if (mkdir(mutable_path, DIR_PERMISSIONS) != 0 && errno != EEXIST) {
                log_error("Failed to create directory: %s", mutable_path);
                ok = false;
            }
        }
    }

    arena_free(mutable_path);
    return ok;
}

/**
 * Create a symlink for the package in ELM_HOME.
 * Creates: ELM_HOME/packages/author/name/version -> source_path
 */
static bool create_package_symlink(InstallEnv *env, const char *source_path,
                                   const char *author, const char *name, const char *version) {
    /* Build path: packages_dir/author/name */
    size_t base_len = strlen(env->cache->packages_dir) + strlen(author) + strlen(name) + 3;
    char *base_dir = arena_malloc(base_len);
    if (!base_dir) {
        return false;
    }
    snprintf(base_dir, base_len, "%s/%s/%s", env->cache->packages_dir, author, name);

    if (!ensure_path_exists(base_dir)) {
        log_error("Failed to create package directory: %s", base_dir);
        arena_free(base_dir);
        return false;
    }

    /* Build symlink path: packages_dir/author/name/version */
    size_t link_len = base_len + strlen(version) + 2;
    char *link_path = arena_malloc(link_len);
    if (!link_path) {
        arena_free(base_dir);
        return false;
    }
    snprintf(link_path, link_len, "%s/%s", base_dir, version);
    arena_free(base_dir);

    /* Remove existing symlink or directory */
    struct stat st;
    if (lstat(link_path, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            if (unlink(link_path) != 0) {
                log_error("Failed to remove existing symlink: %s", link_path);
                arena_free(link_path);
                return false;
            }
        } else if (S_ISDIR(st.st_mode)) {
            if (!remove_directory_recursive(link_path)) {
                log_error("Failed to remove existing directory: %s", link_path);
                arena_free(link_path);
                return false;
            }
        } else {
            if (unlink(link_path) != 0) {
                log_error("Failed to remove existing file: %s", link_path);
                arena_free(link_path);
                return false;
            }
        }
    }

    /* Create symlink */
    if (symlink(source_path, link_path) != 0) {
        log_error("Failed to create symlink %s -> %s: %s", link_path, source_path, strerror(errno));
        arena_free(link_path);
        return false;
    }

    log_debug("Created symlink: %s -> %s", link_path, source_path);
    arena_free(link_path);
    return true;
}

/**
 * Register the application's elm.json in the dependency tracking directory.
 * Creates: tracking_dir/author/name/version/<hash_of_path>
 */
static bool register_dependency_tracking(const char *author, const char *name, const char *version,
                                         const char *app_elm_json_path) {
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return false;
    }

    /* Build path: tracking_dir/author/name/version */
    size_t dir_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *version_dir = arena_malloc(dir_len);
    if (!version_dir) {
        arena_free(tracking_dir);
        return false;
    }
    snprintf(version_dir, dir_len, "%s/%s/%s/%s", tracking_dir, author, name, version);
    arena_free(tracking_dir);

    if (!ensure_path_exists(version_dir)) {
        log_error("Failed to create tracking directory: %s", version_dir);
        arena_free(version_dir);
        return false;
    }

    /* Get absolute path of the app's elm.json */
    char abs_path[PATH_MAX];
    if (!realpath(app_elm_json_path, abs_path)) {
        log_error("Failed to resolve absolute path for: %s", app_elm_json_path);
        arena_free(version_dir);
        return false;
    }

    /* Create filename from hash of path */
    unsigned long path_hash = hash_path(abs_path);
    char hash_filename[32];
    snprintf(hash_filename, sizeof(hash_filename), "%lx", path_hash);

    /* Build full tracking file path */
    size_t file_len = strlen(version_dir) + strlen(hash_filename) + 2;
    char *tracking_file = arena_malloc(file_len);
    if (!tracking_file) {
        arena_free(version_dir);
        return false;
    }
    snprintf(tracking_file, file_len, "%s/%s", version_dir, hash_filename);
    arena_free(version_dir);

    /* Write the app elm.json path to the tracking file */
    FILE *f = fopen(tracking_file, "w");
    if (!f) {
        log_error("Failed to create tracking file: %s", tracking_file);
        arena_free(tracking_file);
        return false;
    }

    fprintf(f, "%s\n", abs_path);
    fclose(f);

    log_debug("Registered dependency tracking: %s", tracking_file);
    arena_free(tracking_file);
    return true;
}

static bool ensure_local_dev_in_registry_dat(InstallEnv *env, const char *author, const char *name,
                                             const char *version) {
    if (!env || !env->cache || !env->cache->registry_path) {
        log_error("Cannot update registry.dat for local-dev package (missing cache configuration)");
        return false;
    }

    const char *registry_path = env->cache->registry_path;
    Registry *registry = NULL;

    if (file_exists(registry_path)) {
        registry = registry_load_from_dat(registry_path, NULL);
        if (!registry) {
            log_error("Failed to load existing registry.dat from %s", registry_path);
            return false;
        }
    } else {
        registry = registry_create();
        if (!registry) {
            log_error("Failed to allocate registry for local-dev entry");
            return false;
        }
    }

    Version parsed = version_parse(version);

    bool added = false;
    if (!registry_add_version_ex(registry, author, name, parsed, false, &added)) {
        log_error("Failed to add %s/%s %s to registry.dat", author, name, version);
        registry_free(registry);
        return false;
    }

    if (added) {
        registry_sort_entries(registry);
        if (!registry_dat_write(registry, registry_path)) {
            log_error("Failed to write updated registry.dat with local-dev package");
            registry_free(registry);
            return false;
        }
        log_debug("Registered %s/%s %s in registry.dat", author, name, version);
    } else {
        log_debug("Package %s/%s %s already present in registry.dat", author, name, version);
    }

    registry_free(registry);
    return true;
}

static void remove_local_dev_from_registry_dat(InstallEnv *env, const char *author, const char *name,
                                               const char *version) {
    if (!env || !env->cache || !env->cache->registry_path || !author || !name || !version) {
        return;
    }

    const char *registry_path = env->cache->registry_path;
    if (!file_exists(registry_path)) {
        return;
    }

    Registry *registry = registry_load_from_dat(registry_path, NULL);
    if (!registry) {
        log_debug("Failed to load registry.dat for local-dev removal: %s", registry_path);
        return;
    }

    Version parsed = version_parse(version);
    bool removed = false;
    if (!registry_remove_version_ex(registry, author, name, parsed, false, &removed)) {
        registry_free(registry);
        log_debug("Failed to remove %s/%s %s from registry.dat", author, name, version);
        return;
    }

    if (removed) {
        registry_sort_entries(registry);
        if (!registry_dat_write(registry, registry_path)) {
            log_debug("Failed to write registry.dat after local-dev removal: %s", registry_path);
        }
    }

    registry_free(registry);
}

/**
 * Register the local-dev package in the text registry file.
 * This file is used to track all local-dev packages regardless of protocol mode.
 * The file is stored in the local-dev tracking directory, not the V2 repository.
 */
static bool register_local_dev_v2_text_registry(InstallEnv *env, const char *author, const char *name,
                                                const char *version, const char *source_elm_json_path) {
    (void)env;

    /* Get the tracking directory (V2-independent) */
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        log_debug("No tracking directory available for local-dev registry");
        return true; /* Not an error - just skip tracking in registry file */
    }
    
    /* Create the tracking directory if it doesn't exist */
    if (!ensure_path_exists(tracking_dir)) {
        log_debug("Could not create local-dev tracking directory: %s", tracking_dir);
        arena_free(tracking_dir);
        return true; /* Not a fatal error - skip tracking */
    }
    
    /* Build path: tracking_dir/registry-local-dev.dat */
    size_t path_len = strlen(tracking_dir) + strlen("/") + strlen(REGISTRY_LOCAL_DEV_DAT) + 1;
    char *reg_path = arena_malloc(path_len);
    if (!reg_path) {
        arena_free(tracking_dir);
        return true; /* Not a fatal error - skip tracking */
    }
    snprintf(reg_path, path_len, "%s/%s", tracking_dir, REGISTRY_LOCAL_DEV_DAT);
    arena_free(tracking_dir);
    
    GlobalContext *ctx = global_context_get();

    ElmJson *pkg_json = elm_json_read(source_elm_json_path);
    if (!pkg_json) {
        log_error("Failed to read local package elm.json: %s", source_elm_json_path);
        arena_free(reg_path);
        return false;
    }

    char *existing_content = file_read_contents(reg_path);
    bool entry_exists = false;

    if (existing_content) {
        char search_pattern[MAX_PACKAGE_NAME_LENGTH];
        snprintf(search_pattern, sizeof(search_pattern), "package: %s/%s", author, name);
        char *pkg_pos = strstr(existing_content, search_pattern);
        if (pkg_pos) {
            char version_pattern[MAX_VERSION_STRING_MEDIUM_LENGTH];
            snprintf(version_pattern, sizeof(version_pattern), "version: %s", version);
            char *next_pkg = strstr(pkg_pos + 1, "package: ");
            if (next_pkg) {
                size_t search_len = (size_t)(next_pkg - pkg_pos);
                char *section = arena_malloc(search_len + 1);
                if (section) {
                    memcpy(section, pkg_pos, search_len);
                    section[search_len] = '\0';
                    entry_exists = strstr(section, version_pattern) != NULL;
                    arena_free(section);
                }
            } else {
                entry_exists = strstr(pkg_pos, version_pattern) != NULL;
            }
        }
        arena_free(existing_content);
    }

    if (entry_exists) {
        log_debug("Package %s/%s %s already in registry-local-dev.dat", author, name, version);
        elm_json_free(pkg_json);
        arena_free(reg_path);
        return true;
    }

    /* Ensure parent directory exists before creating the file */
    char *parent_dir = arena_strdup(reg_path);
    if (parent_dir) {
        char *last_slash = strrchr(parent_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (!ensure_path_exists(parent_dir)) {
                log_debug("Could not create directory for registry-local-dev.dat: %s", parent_dir);
                arena_free(parent_dir);
                elm_json_free(pkg_json);
                arena_free(reg_path);
                return true; /* Not a fatal error - skip tracking */
            }
        }
        arena_free(parent_dir);
    }

    FILE *f = fopen(reg_path, "a");
    if (!f) {
        log_error("Failed to open %s for appending", reg_path);
        elm_json_free(pkg_json);
        arena_free(reg_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize == 0) {
        const char *compiler_name = global_context_compiler_name();
        const char *compiler_version = (ctx && ctx->compiler_version) ? ctx->compiler_version : "0.19.1";
        fprintf(f, "format 2\n");
        fprintf(f, "%s %s\n\n", compiler_name, compiler_version);
    }

    fprintf(f, "package: %s/%s\n", author, name);
    fprintf(f, "    version: %s\n", version);
    fprintf(f, "    status: valid\n");
    fprintf(f, "    license: BSD-3-Clause\n");
    fprintf(f, "    dependencies:\n");

    if (pkg_json->package_dependencies && pkg_json->package_dependencies->count > 0) {
        for (int i = 0; i < pkg_json->package_dependencies->count; i++) {
            Package *dep = &pkg_json->package_dependencies->packages[i];
            fprintf(f, "        %s/%s  %s\n", dep->author, dep->name,
                    dep->version ? dep->version : "1.0.0 <= v < 2.0.0");
        }
    }
    fprintf(f, "\n");

    fclose(f);
    log_debug("Registered %s/%s %s in registry-local-dev.dat", author, name, version);

    elm_json_free(pkg_json);
    arena_free(reg_path);
    return true;
}

/**
 * Register the local-dev package so solvers can discover it.
 * Always creates registry-local-dev.dat to track local-dev packages (used by
 * `wrap repository local-dev` to list packages). In V2 mode, this is also used
 * by the solver. In V1 mode, the solver uses registry.dat instead.
 */
static bool register_in_local_dev_registry(InstallEnv *env, const char *author, const char *name,
                                           const char *version, const char *source_elm_json_path) {
    /* Always create/update registry-local-dev.dat for tracking purposes */
    bool v2_result = register_local_dev_v2_text_registry(env, author, name, version, source_elm_json_path);

    /* Also update V1 registry.dat so the solver can find it */
    bool registry_dat_result = ensure_local_dev_in_registry_dat(env, author, name, version);
    return v2_result && registry_dat_result;
}

/**
 * Check if the current directory is a package being tracked for local-dev.
 * Returns the package info if found, NULL otherwise.
 */
static bool find_local_dev_package_info(const char *package_elm_json_path,
                                        char **out_author, char **out_name, char **out_version) {
    /* Read the package's elm.json to get author/name */
    ElmJson *pkg_json = elm_json_read(package_elm_json_path);
    if (!pkg_json || pkg_json->type != ELM_PROJECT_PACKAGE) {
        if (pkg_json) elm_json_free(pkg_json);
        return false;
    }

    if (!pkg_json->package_name) {
        elm_json_free(pkg_json);
        return false;
    }

    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(pkg_json->package_name, &author, &name)) {
        elm_json_free(pkg_json);
        return false;
    }
    elm_json_free(pkg_json);

    /* Check if this package is being tracked */
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        arena_free(author);
        arena_free(name);
        return false;
    }

    /* Look for tracking_dir/author/name/ */
    size_t pkg_track_len = strlen(tracking_dir) + strlen(author) + strlen(name) + 3;
    char *pkg_track_dir = arena_malloc(pkg_track_len);
    if (!pkg_track_dir) {
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        return false;
    }
    snprintf(pkg_track_dir, pkg_track_len, "%s/%s/%s", tracking_dir, author, name);
    arena_free(tracking_dir);

    struct stat st;
    if (stat(pkg_track_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        arena_free(pkg_track_dir);
        arena_free(author);
        arena_free(name);
        return false;
    }

    /* Find the version directory (there should be one) */
    DIR *dir = opendir(pkg_track_dir);
    if (!dir) {
        arena_free(pkg_track_dir);
        arena_free(author);
        arena_free(name);
        return false;
    }

    char *version = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        version = arena_strdup(entry->d_name);
        break;
    }
    closedir(dir);
    arena_free(pkg_track_dir);

    if (!version) {
        arena_free(author);
        arena_free(name);
        return false;
    }

    *out_author = author;
    *out_name = name;
    *out_version = version;
    return true;
}


/**
 * Refresh indirect dependencies for an application that depends on a local-dev package.
 * 
 * Since local-dev packages are not in the registry, we can't use the solver to look them up.
 * Instead, we read the local package's elm.json directly and resolve its dependencies
 * one by one (which ARE in the registry).
 */
static bool refresh_app_indirect_deps(const char *app_elm_json_path, InstallEnv *env,
                                      const char *pkg_author, const char *pkg_name,
                                      const char *local_pkg_elm_json_path) {
    log_debug("Refreshing indirect dependencies for: %s", app_elm_json_path);

    /* Read the application's elm.json */
    ElmJson *app_json = elm_json_read(app_elm_json_path);
    if (!app_json) {
        log_error("Failed to read application elm.json: %s", app_elm_json_path);
        return false;
    }

    if (app_json->type != ELM_PROJECT_APPLICATION) {
        log_debug("Skipping non-application project: %s", app_elm_json_path);
        elm_json_free(app_json);
        return true;
    }

    /* Read the local package's elm.json to get its dependencies */
    ElmJson *pkg_json = elm_json_read(local_pkg_elm_json_path);
    if (!pkg_json) {
        log_error("Failed to read local package elm.json: %s", local_pkg_elm_json_path);
        elm_json_free(app_json);
        return false;
    }

    if (pkg_json->type != ELM_PROJECT_PACKAGE) {
        log_error("Local-dev path is not a package project: %s", local_pkg_elm_json_path);
        elm_json_free(pkg_json);
        elm_json_free(app_json);
        return false;
    }

    /* Resolve each dependency of the local package */
    bool changed = false;
    bool resolution_failed = false;

    if (pkg_json->package_dependencies && pkg_json->package_dependencies->count > 0) {
        for (int i = 0; i < pkg_json->package_dependencies->count && !resolution_failed; i++) {
            Package *dep = &pkg_json->package_dependencies->packages[i];

            /* Skip if already present in the app (direct or indirect) */
            bool already_present = find_existing_package(app_json, dep->author, dep->name) != NULL;
            
            if (already_present) {
                log_debug("Dependency %s/%s already present in app", dep->author, dep->name);
                continue;
            }

            /* Check if package exists in registry */
            bool found_in_registry = false;
            if (env->protocol_mode == PROTOCOL_V2 && env->v2_registry) {
                found_in_registry = v2_registry_find(env->v2_registry, dep->author, dep->name) != NULL;
            } else if (env->registry) {
                found_in_registry = registry_find(env->registry, dep->author, dep->name) != NULL;
            }

            if (!found_in_registry) {
                log_error("Dependency %s/%s is not in the registry", dep->author, dep->name);
                log_error("This dependency is required by local package %s/%s", pkg_author, pkg_name);
                resolution_failed = true;
                break;
            }

            /* Use solver to resolve this dependency */
            SolverState *solver = solver_init(env, install_env_solver_online(env));
            if (!solver) {
                log_error("Failed to initialize solver for %s/%s", dep->author, dep->name);
                resolution_failed = true;
                break;
            }

            InstallPlan *dep_plan = NULL;
            SolverResult result = solver_add_package(solver, app_json, dep->author, dep->name, NULL, false, false, false, &dep_plan);
            solver_free(solver);

            if (result != SOLVER_OK) {
                log_error("Failed to resolve dependency %s/%s", dep->author, dep->name);
                if (dep_plan) install_plan_free(dep_plan);
                resolution_failed = true;
                break;
            }

            /* Apply resolved dependencies to app_json */
            if (dep_plan) {
                for (int j = 0; j < dep_plan->count; j++) {
                    PackageChange *change = &dep_plan->changes[j];

                    /* Skip if already a direct dependency */
                    if (package_map_find(app_json->dependencies_direct, change->author, change->name) ||
                        package_map_find(app_json->dependencies_test_direct, change->author, change->name)) {
                        continue;
                    }

                    /* Check if this is a new or updated indirect dependency */
                    Package *existing = package_map_find(app_json->dependencies_indirect, change->author, change->name);
                    if (!existing) {
                        existing = package_map_find(app_json->dependencies_test_indirect, change->author, change->name);
                    }

                    if (!existing) {
                        /* New indirect dependency */
                        package_map_add(app_json->dependencies_indirect, change->author, change->name, change->new_version);
                        changed = true;
                        log_debug("Added indirect dependency: %s/%s %s", change->author, change->name, change->new_version);
                    } else if (change->new_version && strcmp(existing->version, change->new_version) != 0) {
                        /* Update existing indirect dependency version */
                        arena_free(existing->version);
                        existing->version = arena_strdup(change->new_version);
                        changed = true;
                        log_debug("Updated indirect dependency: %s/%s %s", change->author, change->name, change->new_version);
                    }
                }
                install_plan_free(dep_plan);
            }
        }
    }

    elm_json_free(pkg_json);

    if (resolution_failed) {
        elm_json_free(app_json);
        return false;
    }

    if (changed) {
        if (!elm_json_write(app_json, app_elm_json_path)) {
            log_error("Failed to write updated elm.json: %s", app_elm_json_path);
            elm_json_free(app_json);
            return false;
        }
        printf("Updated indirect dependencies in: %s\n", app_elm_json_path);
    }

    elm_json_free(app_json);
    return true;
}

/**
 * Prune orphaned indirect dependencies from an application that depends on a local-dev package.
 *
 * This function detects indirect dependencies that are no longer reachable from any direct
 * dependency and removes them from the application's elm.json.
 *
 * @param app_elm_json_path Path to the application's elm.json
 * @param cache             Cache config for looking up package elm.json files
 * @return true on success, false on error
 */
static bool prune_app_orphaned_deps(const char *app_elm_json_path, CacheConfig *cache) {
    log_debug("Pruning orphaned dependencies for: %s", app_elm_json_path);

    /* Read the application's elm.json */
    ElmJson *app_json = elm_json_read(app_elm_json_path);
    if (!app_json) {
        log_error("Failed to read application elm.json: %s", app_elm_json_path);
        return false;
    }

    if (app_json->type != ELM_PROJECT_APPLICATION) {
        log_debug("Skipping non-application project: %s", app_elm_json_path);
        elm_json_free(app_json);
        return true;
    }

    /* Use the shared orphan detection function (no exclusions) */
    PackageMap *orphaned = NULL;
    if (!find_orphaned_packages(app_json, (struct CacheConfig *)cache, NULL, NULL, &orphaned)) {
        elm_json_free(app_json);
        return false;
    }

    /* Remove orphaned packages from elm.json */
    bool changed = false;
    if (orphaned && orphaned->count > 0) {
        for (int i = 0; i < orphaned->count; i++) {
            Package *pkg = &orphaned->packages[i];
            log_debug("Removing orphaned: %s/%s", pkg->author, pkg->name);

            /* Remove from indirect dependencies */
            if (app_json->dependencies_indirect &&
                package_map_find(app_json->dependencies_indirect, pkg->author, pkg->name)) {
                package_map_remove(app_json->dependencies_indirect, pkg->author, pkg->name);
                changed = true;
            }
            if (app_json->dependencies_test_indirect &&
                package_map_find(app_json->dependencies_test_indirect, pkg->author, pkg->name)) {
                package_map_remove(app_json->dependencies_test_indirect, pkg->author, pkg->name);
                changed = true;
            }
        }
        package_map_free(orphaned);
    }

    if (changed) {
        if (!elm_json_write(app_json, app_elm_json_path)) {
            log_error("Failed to write updated elm.json: %s", app_elm_json_path);
            elm_json_free(app_json);
            return false;
        }
        printf("Pruned orphaned dependencies in: %s\n", app_elm_json_path);
    }

    elm_json_free(app_json);
    return true;
}

int refresh_local_dev_dependents(InstallEnv *env) {
    /* Check if we're in a package directory that's being tracked */
    char *author = NULL;
    char *name = NULL;
    char *version = NULL;

    if (!find_local_dev_package_info("elm.json", &author, &name, &version)) {
        /* Not a tracked local-dev package, nothing to do */
        return 0;
    }

    log_debug("Found local-dev package: %s/%s %s", author, name, version);

    /* Get all dependent applications */
    int dep_count = 0;
    char **dep_paths = local_dev_get_tracking_apps(author, name, version, &dep_count);

    if (dep_count == 0 || !dep_paths) {
        log_debug("No dependent applications to refresh");
        arena_free(author);
        arena_free(name);
        arena_free(version);
        return 0;
    }

    printf("Refreshing %d dependent application(s)...\n", dep_count);

    int failed = 0;
    for (int i = 0; i < dep_count; i++) {
        /* We're in the local package directory, so elm.json is right here */
        if (!refresh_app_indirect_deps(dep_paths[i], env, author, name, "elm.json")) {
            log_error("Failed to refresh: %s", dep_paths[i]);
            failed++;
        }
        arena_free(dep_paths[i]);
    }
    arena_free(dep_paths);

    arena_free(author);
    arena_free(name);
    arena_free(version);

    return failed > 0 ? 1 : 0;
}

int prune_local_dev_dependents(CacheConfig *cache) {
    /* Check if we're in a package directory that's being tracked */
    char *author = NULL;
    char *name = NULL;
    char *version = NULL;

    if (!find_local_dev_package_info("elm.json", &author, &name, &version)) {
        /* Not a tracked local-dev package, nothing to do */
        return 0;
    }

    log_debug("Found local-dev package: %s/%s %s", author, name, version);

    /* Get all dependent applications */
    int dep_count = 0;
    char **dep_paths = local_dev_get_tracking_apps(author, name, version, &dep_count);

    if (dep_count == 0 || !dep_paths) {
        log_debug("No dependent applications to prune");
        arena_free(author);
        arena_free(name);
        arena_free(version);
        return 0;
    }

    printf("Pruning orphaned dependencies in %d dependent application(s)...\n", dep_count);

    int failed = 0;
    for (int i = 0; i < dep_count; i++) {
        if (!prune_app_orphaned_deps(dep_paths[i], cache)) {
            log_error("Failed to prune orphaned deps: %s", dep_paths[i]);
            failed++;
        }
        arena_free(dep_paths[i]);
    }
    arena_free(dep_paths);

    arena_free(author);
    arena_free(name);
    arena_free(version);

    return failed > 0 ? 1 : 0;
}

int register_local_dev_package(const char *source_path, const char *package_name,
                               InstallEnv *env, bool auto_yes, bool quiet) {
    struct stat st;
    char resolved_source[PATH_MAX];

    /* Resolve source path to absolute path */
    if (!realpath(source_path, resolved_source)) {
        log_error("Failed to resolve source path: %s", source_path);
        return 1;
    }

    /* Verify source is a directory */
    if (stat(resolved_source, &st) != 0 || !S_ISDIR(st.st_mode)) {
        log_error("Source path is not a directory: %s", resolved_source);
        return 1;
    }

    /* Check for elm.json in source directory */
    char source_elm_json[PATH_MAX];
    snprintf(source_elm_json, sizeof(source_elm_json), "%s/elm.json", resolved_source);
    if (stat(source_elm_json, &st) != 0) {
        log_error("No elm.json found in source directory: %s", resolved_source);
        return 1;
    }

    /* Read package info from source elm.json */
    char *actual_author = NULL;
    char *actual_name = NULL;
    char *actual_version = NULL;
    if (!read_package_info_from_elm_json(source_elm_json, &actual_author, &actual_name, &actual_version)) {
        log_error("Failed to read package info from: %s", source_elm_json);
        return 1;
    }

    /* Verify this is actually a package (not an application) */
    ElmJson *pkg_json = elm_json_read(source_elm_json);
    if (!pkg_json) {
        log_error("Failed to read elm.json: %s", source_elm_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }

    if (pkg_json->type != ELM_PROJECT_PACKAGE) {
        log_error("Source is not a package project: %s", resolved_source);
        elm_json_free(pkg_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }
    elm_json_free(pkg_json);

    /* If package_name specified, verify it matches */
    if (package_name) {
        char *spec_author = NULL;
        char *spec_name = NULL;
        if (!parse_package_name(package_name, &spec_author, &spec_name)) {
            arena_free(actual_author);
            arena_free(actual_name);
            arena_free(actual_version);
            return 1;
        }

        if (strcmp(spec_author, actual_author) != 0 || strcmp(spec_name, actual_name) != 0) {
            log_error("Package name mismatch: specified %s/%s but elm.json has %s/%s",
                      spec_author, spec_name, actual_author, actual_name);
            arena_free(spec_author);
            arena_free(spec_name);
            arena_free(actual_author);
            arena_free(actual_name);
            arena_free(actual_version);
            return 1;
        }
        arena_free(spec_author);
        arena_free(spec_name);
    }

    /* Use version from elm.json */
    const char *version = actual_version;
    log_debug("Using local-dev version from elm.json: %s", version);

    /* Show plan */
    if (!quiet) {
        printf("Here is my plan:\n");
        printf("  \n");
        printf("  Register (local-dev):\n");
        printf("    %s/%s    %s (local)\n", actual_author, actual_name, version);
        printf("  \n");
        printf("  Source: %s\n", resolved_source);
        printf("  \n");
        printf("To use this package in an application, run from the application directory:\n");
        printf("    %s package install %s/%s\n", global_context_program_name(), actual_author, actual_name);
        printf("  \n");
    }

    if (!auto_yes) {
        printf("\nWould you like me to proceed? [Y/n]: ");
        fflush(stdout);

        char response[10];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
            printf("Aborted.\n");
            arena_free(actual_author);
            arena_free(actual_name);
            arena_free(actual_version);
            return 0;
        }
    }

    /* Create symlink in ELM_HOME */
    if (!create_package_symlink(env, resolved_source, actual_author, actual_name, version)) {
        log_error("Failed to register the package in the package cache");
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }

    /* Register in registry-local-dev.dat so the solver can find this package */
    if (!register_in_local_dev_registry(env, actual_author, actual_name, version, source_elm_json)) {
        log_error("Warning: Failed to register in local-dev registry");
        /* Continue anyway - the symlink was created successfully */
    }

    if (!quiet) {
        printf("Successfully registered %s/%s %s (local)!\n", actual_author, actual_name, version);
    }

    arena_free(actual_author);
    arena_free(actual_name);
    arena_free(actual_version);

    return 0;
}

int install_local_dev(const char *source_path, const char *package_name,
                      const char *target_elm_json, InstallEnv *env,
                      bool is_test, bool auto_yes) {
    struct stat st;
    char resolved_source[PATH_MAX];

    /* Resolve source path to absolute path */
    if (!realpath(source_path, resolved_source)) {
        log_error("Failed to resolve source path: %s", source_path);
        return 1;
    }

    /* Verify source is a directory */
    if (stat(resolved_source, &st) != 0 || !S_ISDIR(st.st_mode)) {
        log_error("Source path is not a directory: %s", resolved_source);
        return 1;
    }

    /* Check for elm.json in source directory */
    char source_elm_json[PATH_MAX];
    snprintf(source_elm_json, sizeof(source_elm_json), "%s/elm.json", resolved_source);
    if (stat(source_elm_json, &st) != 0) {
        log_error("No elm.json found in source directory: %s", resolved_source);
        return 1;
    }

    /* Read package info from source elm.json */
    char *actual_author = NULL;
    char *actual_name = NULL;
    char *actual_version = NULL;
    if (!read_package_info_from_elm_json(source_elm_json, &actual_author, &actual_name, &actual_version)) {
        log_error("Failed to read package info from: %s", source_elm_json);
        return 1;
    }

    /* If package_name specified, verify it matches */
    if (package_name) {
        char *spec_author = NULL;
        char *spec_name = NULL;
        if (!parse_package_name(package_name, &spec_author, &spec_name)) {
            arena_free(actual_author);
            arena_free(actual_name);
            arena_free(actual_version);
            return 1;
        }

        if (strcmp(spec_author, actual_author) != 0 || strcmp(spec_name, actual_name) != 0) {
            log_error("Package name mismatch: specified %s/%s but elm.json has %s/%s",
                      spec_author, spec_name, actual_author, actual_name);
            arena_free(spec_author);
            arena_free(spec_name);
            arena_free(actual_author);
            arena_free(actual_name);
            arena_free(actual_version);
            return 1;
        }
        arena_free(spec_author);
        arena_free(spec_name);
    }

    /* Use version from elm.json */
    const char *version = actual_version;
    log_debug("Using local-dev version from elm.json: %s", version);

    /* Read target application's elm.json */
    ElmJson *app_json = elm_json_read(target_elm_json);
    if (!app_json) {
        log_error("Failed to read target elm.json: %s", target_elm_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }

    /* Check if package already exists */
    Package *existing = find_existing_package(app_json, actual_author, actual_name);
    bool is_update = (existing != NULL);

    /* 
     * PHASE 1: Resolve all transitive dependencies BEFORE showing the plan.
     * This lets us show a complete install plan and fail early if deps can't resolve.
     */
    
    /* Read the local package's elm.json to get its dependencies */
    ElmJson *pkg_json = elm_json_read(source_elm_json);
    if (!pkg_json) {
        log_error("Failed to read local package elm.json: %s", source_elm_json);
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }
    
    if (pkg_json->type != ELM_PROJECT_PACKAGE) {
        log_error("Local path is not a package project");
        elm_json_free(pkg_json);
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }
    
    /* Collect all install plan changes for display */
    int plan_capacity = 32;
    int plan_count = 0;
    PackageChange *plan_changes = arena_malloc(plan_capacity * sizeof(PackageChange));
    if (!plan_changes) {
        log_error("Out of memory");
        elm_json_free(pkg_json);
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        return 1;
    }
    
    /* Process each dependency of the local package to build the install plan */
    bool resolution_failed = false;
    if (pkg_json->package_dependencies && pkg_json->package_dependencies->count > 0) {
        log_progress("Resolving dependencies from local package...");
        
        for (int i = 0; i < pkg_json->package_dependencies->count; i++) {
            Package *dep = &pkg_json->package_dependencies->packages[i];
            
            /* Check if this dependency is already in the app */
            bool already_present = find_existing_package(app_json, dep->author, dep->name) != NULL;
            
            if (already_present) {
                log_debug("Dependency %s/%s already present in app", dep->author, dep->name);
                continue;
            }
            
            /* Check if package exists in registry at all */
            bool found_in_registry = false;
            if (env->protocol_mode == PROTOCOL_V2 && env->v2_registry) {
                found_in_registry = v2_registry_find(env->v2_registry, dep->author, dep->name) != NULL;
            } else if (env->registry) {
                found_in_registry = registry_find(env->registry, dep->author, dep->name) != NULL;
            }
            
            if (!found_in_registry) {
                log_error("Dependency %s/%s is not in the registry", dep->author, dep->name);
                log_error("This dependency is required by local package %s/%s", actual_author, actual_name);
                log_error("You may need to install it with --local-dev first");
                resolution_failed = true;
                break;
            }
            
            /* Use solver to resolve this dependency */
            SolverState *solver = solver_init(env, install_env_solver_online(env));
            if (!solver) {
                log_error("Failed to initialize solver for %s/%s", dep->author, dep->name);
                resolution_failed = true;
                break;
            }
            
            InstallPlan *dep_plan = NULL;
            SolverResult result = solver_add_package(solver, app_json, dep->author, dep->name, NULL, is_test, false, false, &dep_plan);
            solver_free(solver);
            
            if (result != SOLVER_OK) {
                log_error("Failed to resolve dependency %s/%s (constraint: %s)", 
                          dep->author, dep->name, dep->version ? dep->version : "any");
                log_error("This dependency is required by local package %s/%s", actual_author, actual_name);
                if (dep_plan) install_plan_free(dep_plan);
                resolution_failed = true;
                break;
            }
            
            /* Add resolved dependencies to our plan and to app_json (for subsequent solver calls) */
            if (dep_plan && app_json->type == ELM_PROJECT_APPLICATION) {
                for (int j = 0; j < dep_plan->count; j++) {
                    PackageChange *change = &dep_plan->changes[j];
                    
                    /* Check if already in direct deps (don't demote to indirect) */
                    if (package_map_find(app_json->dependencies_direct, change->author, change->name) ||
                        package_map_find(app_json->dependencies_test_direct, change->author, change->name)) {
                        continue;
                    }
                    
                    /* Check if already in plan */
                    bool in_plan = false;
                    for (int k = 0; k < plan_count; k++) {
                        if (strcmp(plan_changes[k].author, change->author) == 0 &&
                            strcmp(plan_changes[k].name, change->name) == 0) {
                            in_plan = true;
                            break;
                        }
                    }
                    if (in_plan) continue;
                    
                    /* Add to install plan for display */
                    if (plan_count >= plan_capacity) {
                        plan_capacity *= 2;
                        plan_changes = arena_realloc(plan_changes, plan_capacity * sizeof(PackageChange));
                    }
                    plan_changes[plan_count].author = arena_strdup(change->author);
                    plan_changes[plan_count].name = arena_strdup(change->name);
                    plan_changes[plan_count].old_version = change->old_version ? arena_strdup(change->old_version) : NULL;
                    plan_changes[plan_count].new_version = arena_strdup(change->new_version);
                    plan_count++;
                    
                    /* Add or update in app_json indirect deps (for subsequent solver calls) */
                    Package *exist = package_map_find(app_json->dependencies_indirect, change->author, change->name);
                    if (!exist) {
                        exist = package_map_find(app_json->dependencies_test_indirect, change->author, change->name);
                    }
                    
                    if (exist) {
                        if (strcmp(exist->version, change->new_version) != 0) {
                            arena_free(exist->version);
                            exist->version = arena_strdup(change->new_version);
                        }
                    } else {
                        PackageMap *indirect_map = is_test ? app_json->dependencies_test_indirect : app_json->dependencies_indirect;
                        package_map_add(indirect_map, change->author, change->name, change->new_version);
                    }
                }
            }
            
            if (dep_plan) install_plan_free(dep_plan);
        }
    }
    
    elm_json_free(pkg_json);
    
    if (resolution_failed) {
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        arena_free(plan_changes);
        return 1;
    }
    
    /* 
     * PHASE 2: Show complete plan and ask for confirmation
     */
    printf("Here is my plan:\n");
    printf("  \n");
    if (is_update) {
        printf("  Change (local):\n");
        printf("    %s/%s    %s => %s (local)\n", actual_author, actual_name, existing->version, version);
    } else {
        printf("  Add (local):\n");
        printf("    %s/%s    %s (local)\n", actual_author, actual_name, version);
    }
    
    if (plan_count > 0) {
        printf("  \n");
        printf("  Add (indirect dependencies):\n");
        for (int i = 0; i < plan_count; i++) {
            if (plan_changes[i].old_version) {
                printf("    %s/%s    %s => %s\n", 
                       plan_changes[i].author, plan_changes[i].name,
                       plan_changes[i].old_version, plan_changes[i].new_version);
            } else {
                printf("    %s/%s    %s\n", 
                       plan_changes[i].author, plan_changes[i].name,
                       plan_changes[i].new_version);
            }
        }
    }
    
    printf("  \n");
    printf("  Source: %s\n", resolved_source);
    printf("  \n");

    if (!auto_yes) {
        printf("\nWould you like me to proceed? [Y/n]: ");
        fflush(stdout);

        char response[10];
        if (!fgets(response, sizeof(response), stdin) ||
            (response[0] != 'Y' && response[0] != 'y' && response[0] != '\n')) {
            printf("Aborted.\n");
            elm_json_free(app_json);
            arena_free(actual_author);
            arena_free(actual_name);
            arena_free(actual_version);
            arena_free(plan_changes);
            return 0;
        }
    }

    /*
     * PHASE 3: Apply the changes
     */
    
    /* Create symlink in ELM_HOME */
    if (!create_package_symlink(env, resolved_source, actual_author, actual_name, version)) {
        log_error("Failed to register the package in the package cache.");
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        arena_free(plan_changes);
        return 1;
    }

    /* Register in registry-local-dev.dat so the solver can find this package */
    if (!register_in_local_dev_registry(env, actual_author, actual_name, version, source_elm_json)) {
        log_error("Warning: Failed to register in local-dev registry");
        /* Continue anyway - the symlink was created successfully */
    }

    /* Add or update the local-dev package in elm.json.
     * For packages, version_to_constraint is applied automatically.
     * For updates, the existing entry is modified in place. */
    if (!add_or_update_package_in_elm_json(app_json, actual_author, actual_name, version,
                                           is_test, true /* is_direct */, false /* remove_first */)) {
        log_error("Failed to add %s/%s to elm.json", actual_author, actual_name);
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        arena_free(plan_changes);
        return 1;
    }

    /* Write updated elm.json */
    printf("Saving elm.json...\n");
    if (!elm_json_write(app_json, target_elm_json)) {
        log_error("Failed to write elm.json");
        elm_json_free(app_json);
        arena_free(actual_author);
        arena_free(actual_name);
        arena_free(actual_version);
        arena_free(plan_changes);
        return 1;
    }

    /* Register in dependency tracking */
    if (!register_dependency_tracking(actual_author, actual_name, version, target_elm_json)) {
        log_error("Warning: Failed to register dependency tracking");
        /* Continue anyway - the install succeeded */
    }

    printf("Successfully installed %s/%s %s (local)!\n", actual_author, actual_name, version);

    elm_json_free(app_json);
    arena_free(actual_author);
    arena_free(actual_name);
    arena_free(actual_version);
    arena_free(plan_changes);

    return 0;
}

int unregister_local_dev_package(InstallEnv *env) {
    /* Read the current directory's elm.json to get package info */
    ElmJson *pkg_json = elm_json_read("elm.json");
    if (!pkg_json) {
        log_error("Could not read elm.json in current directory");
        return 1;
    }

    if (pkg_json->type != ELM_PROJECT_PACKAGE) {
        log_error("Current directory is not an Elm package");
        elm_json_free(pkg_json);
        return 1;
    }

    if (!pkg_json->package_name) {
        log_error("Package name not found in elm.json");
        elm_json_free(pkg_json);
        return 1;
    }

    char *author = NULL;
    char *name = NULL;
    if (!parse_package_name(pkg_json->package_name, &author, &name)) {
        log_error("Invalid package name in elm.json: %s", pkg_json->package_name);
        elm_json_free(pkg_json);
        return 1;
    }

    const char *version = pkg_json->package_version ? pkg_json->package_version : "1.0.0";

    /* Get the tracking directory */
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        log_error("Could not determine tracking directory");
        arena_free(author);
        arena_free(name);
        elm_json_free(pkg_json);
        return 1;
    }

    /* Build path: tracking_dir/author/name/version */
    size_t path_len = strlen(tracking_dir) + strlen(author) + strlen(name) + strlen(version) + 4;
    char *pkg_path = arena_malloc(path_len);
    if (!pkg_path) {
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        elm_json_free(pkg_json);
        return 1;
    }
    snprintf(pkg_path, path_len, "%s/%s/%s/%s", tracking_dir, author, name, version);

    struct stat st;
    if (stat(pkg_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("No local-dev tracking found for %s/%s %s\n", author, name, version);
        remove_local_dev_from_registry_dat(env, author, name, version);
        arena_free(pkg_path);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        elm_json_free(pkg_json);
        return 0;
    }

    if (remove_directory_recursive(pkg_path)) {
        printf("Removed local-dev tracking for %s/%s %s\n", author, name, version);
        remove_local_dev_from_registry_dat(env, author, name, version);
        arena_free(pkg_path);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        elm_json_free(pkg_json);
        return 0;
    } else {
        log_error("Failed to remove tracking for %s/%s %s", author, name, version);
        arena_free(pkg_path);
        arena_free(tracking_dir);
        arena_free(author);
        arena_free(name);
        elm_json_free(pkg_json);
        return 1;
    }
}

/**
 * Check if a package version is registered in the local-dev registry.
 * If so, register the application as a dependent for tracking.
 *
 * This is called after a regular `wrap install` command successfully installs
 * a package, to ensure that if the package is a local-dev package, the
 * application gets registered for tracking updates.
 *
 * @param author            Package author
 * @param name              Package name
 * @param version           Package version
 * @param app_elm_json_path Path to the application's elm.json
 * @return true on success (or if package is not local-dev), false on error
 */
bool register_local_dev_tracking_if_needed(const char *author, const char *name,
                                           const char *version, const char *app_elm_json_path) {
    if (!author || !name || !version || !app_elm_json_path) {
        return true; /* Not an error - just nothing to do */
    }

    /* Get the local-dev registry path */
    char *tracking_dir = get_local_dev_tracking_dir();
    if (!tracking_dir) {
        return true; /* No tracking dir means no local-dev packages */
    }

    /* Build path to registry-local-dev.dat */
    size_t reg_path_len = strlen(tracking_dir) + strlen("/") + strlen(REGISTRY_LOCAL_DEV_DAT) + 1;
    char *reg_path = arena_malloc(reg_path_len);
    if (!reg_path) {
        arena_free(tracking_dir);
        return true; /* Not a fatal error */
    }
    snprintf(reg_path, reg_path_len, "%s/%s", tracking_dir, REGISTRY_LOCAL_DEV_DAT);

    /* Check if the registry file exists */
    struct stat st;
    if (stat(reg_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        arena_free(reg_path);
        arena_free(tracking_dir);
        return true; /* No local-dev registry - nothing to do */
    }

    /* Load the local-dev registry */
    V2Registry *local_dev_registry = v2_registry_load_from_text(reg_path);
    arena_free(reg_path);

    if (!local_dev_registry) {
        arena_free(tracking_dir);
        return true; /* Failed to load registry - not a fatal error */
    }

    /* Parse version string */
    Version parsed_v;
    if (!version_parse_safe(version, &parsed_v)) {
        v2_registry_free(local_dev_registry);
        arena_free(tracking_dir);
        return true; /* Invalid version format - not a fatal error */
    }

    /* Check if this package/version is in the local-dev registry */
    V2PackageVersion *pkg_version = v2_registry_find_version(local_dev_registry, author, name,
                                                              parsed_v.major, parsed_v.minor,
                                                              parsed_v.patch);
    if (!pkg_version) {
        v2_registry_free(local_dev_registry);
        arena_free(tracking_dir);
        return true; /* Package not in local-dev registry - nothing to do */
    }

    v2_registry_free(local_dev_registry);

    /* Package is in local-dev registry - register the tracking */
    log_debug("Package %s/%s %s is a local-dev package, registering tracking for %s",
              author, name, version, app_elm_json_path);

    bool result = register_dependency_tracking(author, name, version, app_elm_json_path);
    arena_free(tracking_dir);

    if (result) {
        log_debug("Registered local-dev tracking for %s/%s %s -> %s",
                  author, name, version, app_elm_json_path);
    } else {
        log_debug("Failed to register local-dev tracking for %s/%s %s",
                  author, name, version);
    }

    return result;
}
