/**
 * protocol_v1/install.c - V1 Protocol Install Functions Implementation
 *
 * Functions for package dependency display using the V1 protocol.
 * These functions require network access and package downloads to read
 * package elm.json files from the cache.
 */

#include "install.h"
#include "../elm_json.h"
#include "../cache.h"
#include "../constants.h"
#include "../registry.h"
#include "../shared/log.h"
#include "../alloc.h"
#include "../fileutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#define ELM_JSON_PATH "elm.json"

typedef struct {
    char *author;
    char *name;
    char *version;
} V1CycleNode;

typedef struct {
    V1CycleNode *items;
    int count;
    int capacity;
} V1CycleNodeList;

typedef struct {
    InstallEnv *env;
    V1CycleNodeList stack;
    V1CycleNodeList visited;
    int report_count;
} V1CycleState;

/**
 * Helper function to find elm.json in a package directory.
 * Handles both cases: elm.json at root and elm.json in a subdirectory.
 */
static char* v1_find_package_elm_json(const char *pkg_path) {
    /* Try direct path first */
    size_t direct_len = strlen(pkg_path) + strlen("/elm.json") + 1;
    char *direct_path = arena_malloc(direct_len);
    if (!direct_path) return NULL;
    snprintf(direct_path, direct_len, "%s/elm.json", pkg_path);

    struct stat st;
    if (stat(direct_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return direct_path;
    }
    arena_free(direct_path);

    /* Not found at root - look in subdirectories */
    DIR *dir = opendir(pkg_path);
    if (!dir) return NULL;

    struct dirent *entry;
    char *found_path = NULL;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
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

static bool v1_cycle_node_equals(const V1CycleNode *node,
                                 const char *author,
                                 const char *name,
                                 const char *version) {
    return node &&
           node->author &&
           node->name &&
           node->version &&
           author &&
           name &&
           version &&
           strcmp(node->author, author) == 0 &&
           strcmp(node->name, name) == 0 &&
           strcmp(node->version, version) == 0;
}

static void v1_cycle_node_list_free(V1CycleNodeList *list) {
    if (!list || !list->items) {
        return;
    }

    for (int i = 0; i < list->count; i++) {
        arena_free(list->items[i].author);
        arena_free(list->items[i].name);
        arena_free(list->items[i].version);
    }

    arena_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool v1_cycle_node_list_push(V1CycleNodeList *list,
                                    const char *author,
                                    const char *name,
                                    const char *version) {
    if (!list || !author || !name || !version) {
        return false;
    }

    if (list->count >= list->capacity) {
        int new_capacity = list->capacity > 0 ? list->capacity * 2 : INITIAL_SMALL_CAPACITY;
        V1CycleNode *new_items = arena_realloc(list->items, (size_t)new_capacity * sizeof(V1CycleNode));
        if (!new_items) {
            return false;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    V1CycleNode *node = &list->items[list->count];
    node->author = arena_strdup(author);
    node->name = arena_strdup(name);
    node->version = arena_strdup(version);
    if (!node->author || !node->name || !node->version) {
        arena_free(node->author);
        arena_free(node->name);
        arena_free(node->version);
        node->author = NULL;
        node->name = NULL;
        node->version = NULL;
        return false;
    }

    list->count++;
    return true;
}

static void v1_cycle_node_list_pop(V1CycleNodeList *list) {
    if (!list || list->count <= 0) {
        return;
    }

    V1CycleNode *node = &list->items[list->count - 1];
    arena_free(node->author);
    arena_free(node->name);
    arena_free(node->version);
    node->author = NULL;
    node->name = NULL;
    node->version = NULL;
    list->count--;
}

static int v1_cycle_node_list_find(const V1CycleNodeList *list,
                                   const char *author,
                                   const char *name,
                                   const char *version) {
    if (!list) {
        return -1;
    }

    for (int i = 0; i < list->count; i++) {
        if (v1_cycle_node_equals(&list->items[i], author, name, version)) {
            return i;
        }
    }

    return -1;
}

static bool v1_cycle_node_list_contains(const V1CycleNodeList *list,
                                        const char *author,
                                        const char *name,
                                        const char *version) {
    return v1_cycle_node_list_find(list, author, name, version) >= 0;
}

static char *v1_resolve_dependency_version(const Package *dep, InstallEnv *env) {
    if (!dep || !dep->author || !dep->name || !dep->version || !env || !env->registry) {
        return NULL;
    }

    if (registry_is_version_constraint(dep->version)) {
        Version resolved;
        if (!registry_resolve_constraint(env->registry,
                                         dep->author,
                                         dep->name,
                                         dep->version,
                                         &resolved)) {
            return NULL;
        }
        return version_to_string(&resolved);
    }

    return arena_strdup(dep->version);
}

static ElmJson *v1_read_package_json_exact(const char *author,
                                           const char *name,
                                           const char *version,
                                           InstallEnv *env) {
    if (!author || !name || !version || !env || !env->cache) {
        return NULL;
    }

    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        return NULL;
    }

    char *elm_json_path = v1_find_package_elm_json(pkg_path);
    ElmJson *elm_json = NULL;

    if (elm_json_path) {
        elm_json = elm_json_read(elm_json_path);
        arena_free(elm_json_path);
    }

    if (!elm_json && !env->offline && cache_download_package_with_env(env, author, name, version)) {
        elm_json_path = v1_find_package_elm_json(pkg_path);
        if (elm_json_path) {
            elm_json = elm_json_read(elm_json_path);
            arena_free(elm_json_path);
        }
    }

    arena_free(pkg_path);
    return elm_json;
}

static void v1_print_cycle_report(V1CycleState *state,
                                  int cycle_start,
                                  const char *author,
                                  const char *name,
                                  const char *version) {
    if (!state || state->report_count >= MAX_PACKAGE_CYCLE_REPORTS) {
        return;
    }

    fprintf(stderr, "\nWarning: package dependency cycle detected:\n\n");

    for (int i = cycle_start; i < state->stack.count; i++) {
        V1CycleNode *node = &state->stack.items[i];
        fprintf(stderr, "    %s/%s %s\n", node->author, node->name, node->version);
        fprintf(stderr, "      ->\n");
    }

    fprintf(stderr, "    %s/%s %s\n\n", author, name, version);
    fprintf(stderr, "This can make Elm dependency verification block while building package artifacts.\n\n");
    state->report_count++;
}

static bool v1_cycle_visit(V1CycleState *state,
                           const char *author,
                           const char *name,
                           const char *version);

static bool v1_cycle_visit_map(V1CycleState *state, PackageMap *deps) {
    if (!state || !deps) {
        return true;
    }

    for (int i = 0; i < deps->count; i++) {
        if (state->report_count >= MAX_PACKAGE_CYCLE_REPORTS) {
            return true;
        }

        Package *dep = &deps->packages[i];
        char *dep_version = v1_resolve_dependency_version(dep, state->env);
        if (!dep_version) {
            continue;
        }

        bool ok = v1_cycle_visit(state, dep->author, dep->name, dep_version);
        arena_free(dep_version);
        if (!ok) {
            return false;
        }
    }

    return true;
}

static bool v1_cycle_visit(V1CycleState *state,
                           const char *author,
                           const char *name,
                           const char *version) {
    if (!state || !author || !name || !version) {
        return false;
    }

    int stack_index = v1_cycle_node_list_find(&state->stack, author, name, version);
    if (stack_index >= 0) {
        v1_print_cycle_report(state, stack_index, author, name, version);
        return true;
    }

    if (v1_cycle_node_list_contains(&state->visited, author, name, version)) {
        return true;
    }

    if (state->stack.count >= MAX_PACKAGE_CYCLE_DEPTH) {
        log_debug("Package cycle detection depth limit reached at %s/%s %s",
                  author,
                  name,
                  version);
        return true;
    }

    if (!v1_cycle_node_list_push(&state->stack, author, name, version)) {
        return false;
    }

    ElmJson *elm_json = v1_read_package_json_exact(author, name, version, state->env);
    if (elm_json) {
        if (elm_json->type == ELM_PROJECT_PACKAGE) {
            if (!v1_cycle_visit_map(state, elm_json->package_dependencies)) {
                elm_json_free(elm_json);
                v1_cycle_node_list_pop(&state->stack);
                return false;
            }
            if (!v1_cycle_visit_map(state, elm_json->package_test_dependencies)) {
                elm_json_free(elm_json);
                v1_cycle_node_list_pop(&state->stack);
                return false;
            }
        }
        elm_json_free(elm_json);
    }

    if (!v1_cycle_node_list_push(&state->visited, author, name, version)) {
        v1_cycle_node_list_pop(&state->stack);
        return false;
    }

    v1_cycle_node_list_pop(&state->stack);
    return true;
}

int v1_report_package_cycles(const char *author, const char *name, const char *version,
                             InstallEnv *env) {
    if (!author || !name || !version || !env || !env->registry || !env->cache) {
        return 0;
    }

    V1CycleState state = {0};
    state.env = env;

    bool ok = v1_cycle_visit(&state, author, name, version);
    if (!ok) {
        log_debug("Package cycle detection failed while scanning %s/%s %s",
                  author,
                  name,
                  version);
    }

    int reports = state.report_count;
    v1_cycle_node_list_free(&state.stack);
    v1_cycle_node_list_free(&state.visited);
    return reports;
}

static int v1_report_package_cycles_for_map(PackageMap *deps,
                                            InstallEnv *env,
                                            PackageMap *checked) {
    if (!deps || !env || !checked) {
        return 0;
    }

    int reports = 0;

    for (int i = 0; i < deps->count; i++) {
        Package *dep = &deps->packages[i];
        if (!dep->author || !dep->name || !dep->version) {
            continue;
        }

        if (package_map_find(checked, dep->author, dep->name)) {
            continue;
        }

        char *dep_version = v1_resolve_dependency_version(dep, env);
        if (!dep_version) {
            continue;
        }

        if (!package_map_add(checked, dep->author, dep->name, dep_version)) {
            arena_free(dep_version);
            continue;
        }

        reports += v1_report_package_cycles(dep->author, dep->name, dep_version, env);
        arena_free(dep_version);

        if (reports > 0) {
            return reports;
        }
    }

    return reports;
}

int v1_report_project_package_cycles(ElmJson *elm_json, InstallEnv *env) {
    if (!elm_json || !env || env->protocol_mode != PROTOCOL_V1) {
        return 0;
    }

    PackageMap *checked = package_map_create();
    if (!checked) {
        return 0;
    }

    int reports = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        reports += v1_report_package_cycles_for_map(elm_json->dependencies_direct, env, checked);
        if (reports == 0) {
            reports += v1_report_package_cycles_for_map(elm_json->dependencies_indirect, env, checked);
        }
        if (reports == 0) {
            reports += v1_report_package_cycles_for_map(elm_json->dependencies_test_direct, env, checked);
        }
        if (reports == 0) {
            reports += v1_report_package_cycles_for_map(elm_json->dependencies_test_indirect, env, checked);
        }
    } else {
        reports += v1_report_package_cycles_for_map(elm_json->package_dependencies, env, checked);
        if (reports == 0) {
            reports += v1_report_package_cycles_for_map(elm_json->package_test_dependencies, env, checked);
        }
    }

    package_map_free(checked);
    return reports;
}

char *v1_package_dependency_constraint(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                                       const char *target_author, const char *target_name,
                                       InstallEnv *env) {
    char *pkg_path = cache_get_package_path(env->cache, pkg_author, pkg_name, pkg_version);
    if (!pkg_path) {
        return NULL;
    }

    char *elm_json_path = v1_find_package_elm_json(pkg_path);
    ElmJson *pkg_elm_json = NULL;

    if (elm_json_path) {
        pkg_elm_json = elm_json_read(elm_json_path);
        arena_free(elm_json_path);
    }

    if (!pkg_elm_json) {
        if (cache_download_package_with_env(env, pkg_author, pkg_name, pkg_version)) {
            elm_json_path = v1_find_package_elm_json(pkg_path);
            if (elm_json_path) {
                pkg_elm_json = elm_json_read(elm_json_path);
                arena_free(elm_json_path);
            }
        }
    }

    arena_free(pkg_path);

    if (!pkg_elm_json) {
        return NULL;
    }

    char *constraint = NULL;

    if (pkg_elm_json->package_dependencies) {
        Package *dep = package_map_find(pkg_elm_json->package_dependencies, target_author, target_name);
        if (dep && dep->version) {
            constraint = arena_strdup(dep->version);
        }
    }

    if (!constraint && pkg_elm_json->package_test_dependencies) {
        Package *dep = package_map_find(pkg_elm_json->package_test_dependencies, target_author, target_name);
        if (dep && dep->version) {
            constraint = arena_strdup(dep->version);
        }
    }

    elm_json_free(pkg_elm_json);
    return constraint;
}

bool v1_package_depends_on(const char *pkg_author, const char *pkg_name, const char *pkg_version,
                           const char *target_author, const char *target_name,
                           InstallEnv *env) {
    char *constraint = v1_package_dependency_constraint(pkg_author, pkg_name, pkg_version,
                                                        target_author, target_name, env);
    bool depends = (constraint != NULL);
    arena_free(constraint);
    return depends;
}

static bool v1_package_depends_on_transitively_visit(const char *pkg_author,
                                                     const char *pkg_name,
                                                     const char *pkg_version,
                                                     const char *target_author,
                                                     const char *target_name,
                                                     InstallEnv *env,
                                                     V1CycleNodeList *visited,
                                                     bool *out_depends);

static bool v1_map_depends_on_transitively(PackageMap *deps,
                                           const char *target_author,
                                           const char *target_name,
                                           InstallEnv *env,
                                           V1CycleNodeList *visited,
                                           bool *out_depends) {
    if (!deps || !target_author || !target_name || !env || !visited || !out_depends) {
        return true;
    }

    for (int i = 0; i < deps->count; i++) {
        Package *dep = &deps->packages[i];
        if (!dep->author || !dep->name || !dep->version) {
            continue;
        }

        if (strcmp(dep->author, target_author) == 0 &&
            strcmp(dep->name, target_name) == 0) {
            *out_depends = true;
            return true;
        }

        char *dep_version = v1_resolve_dependency_version(dep, env);
        if (!dep_version) {
            continue;
        }

        bool ok = v1_package_depends_on_transitively_visit(dep->author,
                                                           dep->name,
                                                           dep_version,
                                                           target_author,
                                                           target_name,
                                                           env,
                                                           visited,
                                                           out_depends);
        arena_free(dep_version);
        if (!ok || *out_depends) {
            return ok;
        }
    }

    return true;
}

static bool v1_package_depends_on_transitively_visit(const char *pkg_author,
                                                     const char *pkg_name,
                                                     const char *pkg_version,
                                                     const char *target_author,
                                                     const char *target_name,
                                                     InstallEnv *env,
                                                     V1CycleNodeList *visited,
                                                     bool *out_depends) {
    if (!pkg_author ||
        !pkg_name ||
        !pkg_version ||
        !target_author ||
        !target_name ||
        !env ||
        !visited ||
        !out_depends) {
        return false;
    }

    if (v1_cycle_node_list_contains(visited, pkg_author, pkg_name, pkg_version)) {
        return true;
    }

    if (!v1_cycle_node_list_push(visited, pkg_author, pkg_name, pkg_version)) {
        return false;
    }

    ElmJson *elm_json = v1_read_package_json_exact(pkg_author, pkg_name, pkg_version, env);
    if (!elm_json) {
        return true;
    }

    bool ok = true;
    if (elm_json->type == ELM_PROJECT_PACKAGE) {
        ok = v1_map_depends_on_transitively(elm_json->package_dependencies,
                                            target_author,
                                            target_name,
                                            env,
                                            visited,
                                            out_depends);
        if (ok && !*out_depends) {
            ok = v1_map_depends_on_transitively(elm_json->package_test_dependencies,
                                                target_author,
                                                target_name,
                                                env,
                                                visited,
                                                out_depends);
        }
    }

    elm_json_free(elm_json);
    return ok;
}

bool v1_package_depends_on_transitively(const char *pkg_author,
                                        const char *pkg_name,
                                        const char *pkg_version,
                                        const char *target_author,
                                        const char *target_name,
                                        InstallEnv *env) {
    if (!pkg_author ||
        !pkg_name ||
        !pkg_version ||
        !target_author ||
        !target_name ||
        !env ||
        !env->cache ||
        !env->registry) {
        return false;
    }

    V1CycleNodeList visited = {0};
    bool depends = false;
    bool ok = v1_package_depends_on_transitively_visit(pkg_author,
                                                       pkg_name,
                                                       pkg_version,
                                                       target_author,
                                                       target_name,
                                                       env,
                                                       &visited,
                                                       &depends);
    if (!ok) {
        log_debug("Transitive dependency check failed while scanning %s/%s %s",
                  pkg_author,
                  pkg_name,
                  pkg_version);
    }

    v1_cycle_node_list_free(&visited);
    return depends;
}

int v1_show_package_dependencies(const char *author, const char *name, const char *version,
                                 InstallEnv *env) {
    char *pkg_path = cache_get_package_path(env->cache, author, name, version);
    if (!pkg_path) {
        log_error("Failed to get package path");
        return 1;
    }

    char *elm_json_path = v1_find_package_elm_json(pkg_path);

    ElmJson *elm_json = NULL;
    if (elm_json_path) {
        elm_json = elm_json_read(elm_json_path);
    }

    if (!elm_json) {
        log_debug("Package not in cache, attempting download");
        if (!cache_download_package_with_env(env, author, name, version)) {
            log_error("Failed to download package %s/%s %s", author, name, version);
            if (elm_json_path) arena_free(elm_json_path);
            arena_free(pkg_path);
            return 1;
        }

        if (elm_json_path) arena_free(elm_json_path);
        elm_json_path = find_package_elm_json(pkg_path);

        if (elm_json_path) {
            elm_json = elm_json_read(elm_json_path);
        }

        if (!elm_json) {
            log_error("Failed to read elm.json for %s/%s %s", author, name, version);
            if (elm_json_path) arena_free(elm_json_path);
            arena_free(pkg_path);
            return 1;
        }
    }

    if (elm_json_path) arena_free(elm_json_path);
    arena_free(pkg_path);

    LogLevel old_level = g_log_level;
    log_set_level(LOG_LEVEL_PROGRESS);

    log_progress("");
    log_progress("Package: %s/%s %s", author, name, version);
    log_progress("========================================");
    log_progress("");

    if (elm_json->type == ELM_PROJECT_PACKAGE && elm_json->package_dependencies) {
        PackageMap *deps = elm_json->package_dependencies;

        if (deps->count == 0) {
            log_progress("No dependencies");
        } else {
            int max_width = 0;
            for (int i = 0; i < deps->count; i++) {
                Package *pkg = &deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            if (elm_json->package_test_dependencies && elm_json->package_test_dependencies->count > 0) {
                PackageMap *test_deps = elm_json->package_test_dependencies;
                for (int i = 0; i < test_deps->count; i++) {
                    Package *pkg = &test_deps->packages[i];
                    int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                    if (pkg_len > max_width) max_width = pkg_len;
                }
            }

            log_progress("Dependencies (%d):", deps->count);
            for (int i = 0; i < deps->count; i++) {
                Package *pkg = &deps->packages[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                log_progress("  %-*s    %s", max_width, pkg_name, pkg->version);
            }
        }

        if (elm_json->package_test_dependencies && elm_json->package_test_dependencies->count > 0) {
            PackageMap *test_deps = elm_json->package_test_dependencies;

            int max_width = 0;
            for (int i = 0; i < test_deps->count; i++) {
                Package *pkg = &test_deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            if (elm_json->package_dependencies) {
                for (int i = 0; i < elm_json->package_dependencies->count; i++) {
                    Package *pkg = &elm_json->package_dependencies->packages[i];
                    int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                    if (pkg_len > max_width) max_width = pkg_len;
                }
            }

            log_progress("");
            log_progress("Test Dependencies (%d):", test_deps->count);
            for (int i = 0; i < test_deps->count; i++) {
                Package *pkg = &test_deps->packages[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                log_progress("  %-*s    %s", max_width, pkg_name, pkg->version);
            }
        }
    } else {
        log_progress("(Not a package - this is an application)");
    }

    log_set_level(old_level);

    v1_report_package_cycles(author, name, version, env);

    /* Check for reverse dependencies in current elm.json */
    ElmJson *current_elm_json = elm_json_read(ELM_JSON_PATH);
    if (current_elm_json) {
        PackageMap *all_deps = package_map_create();

        if (current_elm_json->dependencies_direct) {
            for (int i = 0; i < current_elm_json->dependencies_direct->count; i++) {
                Package *pkg = &current_elm_json->dependencies_direct->packages[i];
                package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (current_elm_json->dependencies_indirect) {
            for (int i = 0; i < current_elm_json->dependencies_indirect->count; i++) {
                Package *pkg = &current_elm_json->dependencies_indirect->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->dependencies_test_direct) {
            for (int i = 0; i < current_elm_json->dependencies_test_direct->count; i++) {
                Package *pkg = &current_elm_json->dependencies_test_direct->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        if (current_elm_json->dependencies_test_indirect) {
            for (int i = 0; i < current_elm_json->dependencies_test_indirect->count; i++) {
                Package *pkg = &current_elm_json->dependencies_test_indirect->packages[i];
                if (!package_map_find(all_deps, pkg->author, pkg->name)) {
                    package_map_add(all_deps, pkg->author, pkg->name, pkg->version);
                }
            }
        }

        /* Note: We skip package_dependencies and package_test_dependencies here because
         * package elm.json files contain version ranges (e.g., "1.0.5 <= v < 2.0.0")
         * rather than concrete versions. We can only check reverse dependencies for
         * concrete versions that can be downloaded from the registry.
         */

        PackageMap *reverse_deps = package_map_create();

        for (int i = 0; i < all_deps->count; i++) {
            Package *pkg = &all_deps->packages[i];

            /* Skip the target package itself */
            if (strcmp(pkg->author, author) == 0 && strcmp(pkg->name, name) == 0) {
                continue;
            }

            if (v1_package_depends_on(pkg->author, pkg->name, pkg->version, author, name, env)) {
                package_map_add(reverse_deps, pkg->author, pkg->name, pkg->version);
            }
        }

        if (reverse_deps->count > 0) {
            /* Calculate max width for aligned output */
            int max_width = 0;
            for (int i = 0; i < reverse_deps->count; i++) {
                Package *pkg = &reverse_deps->packages[i];
                int pkg_len = strlen(pkg->author) + 1 + strlen(pkg->name);
                if (pkg_len > max_width) max_width = pkg_len;
            }

            log_progress("");
            log_progress("Packages in elm.json that depend on %s/%s (%d):", author, name, reverse_deps->count);
            for (int i = 0; i < reverse_deps->count; i++) {
                Package *pkg = &reverse_deps->packages[i];
                char pkg_name[MAX_PACKAGE_NAME_LENGTH];
                snprintf(pkg_name, sizeof(pkg_name), "%s/%s", pkg->author, pkg->name);
                log_progress("  %-*s    %s", max_width, pkg_name, pkg->version);
            }
        }

        package_map_free(reverse_deps);
        package_map_free(all_deps);
        elm_json_free(current_elm_json);
    }

    printf("\n");
    elm_json_free(elm_json);
    return 0;
}
