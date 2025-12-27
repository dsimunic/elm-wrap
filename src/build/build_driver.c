/**
 * build_driver.c - Build driver implementation
 *
 * Generates a complete JSON build plan for Elm compilation.
 */

#include "build_driver.h"
#include "build_types.h"
#include "../elm_json.h"
#include "../elm_project.h"
#include "../install_env.h"
#include "../cache.h"
#include "../fileutil.h"
#include "../alloc.h"
#include "../constants.h"
#include "../dyn_array.h"
#include "../shared/log.h"
#include "../ast/skeleton.h"
#include "../commands/package/package_common.h"
#include "../vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libgen.h>
#include <sys/stat.h>

/* ============================================================================
 * Forward declarations
 * ========================================================================== */

static bool resolve_packages_v2(ElmJson *elm_json, InstallEnv *env,
                                BuildPlan *plan);
static bool compute_package_build_order(BuildPlan *plan, CacheConfig *cache);
static void build_module_package_map(BuildPackage *packages, int pkg_count, CacheConfig *cache);
static bool crawl_modules(const char *project_root, char **src_dirs, int src_dir_count,
                          const char **entry_files, int entry_count, BuildPlan *plan);
static bool compute_module_build_order(BuildPlan *plan);
static void compute_parallel_levels(BuildPlan *plan);
static void group_into_batches(BuildPlan *plan);
static void check_all_package_artifacts(BuildPlan *plan);
static char *path_to_module_name(const char *file_path, const char *src_dir);
static char *make_relative_path(const char *abs_path, const char *base_path);
static int find_module_index(BuildModule *modules, int count, const char *name);
static const char *find_package_for_module(const char *module_name,
                                           BuildPackage *packages, int pkg_count,
                                           CacheConfig *cache);

/* ============================================================================
 * Public API
 * ========================================================================== */

void build_add_problem(BuildPlan *plan, const char *module_name, const char *message) {
    if (!plan) return;

    DYNARRAY_ENSURE_CAPACITY(plan->problems, plan->problem_count,
                             plan->problem_capacity, BuildProblem);

    BuildProblem *prob = &plan->problems[plan->problem_count++];
    prob->module_name = module_name ? arena_strdup(module_name) : NULL;
    prob->message = arena_strdup(message);
}

BuildPlan *build_generate_plan(
    const char *project_root,
    ElmJson *elm_json,
    InstallEnv *env,
    const char **entry_files,
    int entry_count
) {
    BuildPlan *plan = arena_calloc(1, sizeof(BuildPlan));
    if (!plan) return NULL;

    /* Initialize capacities */
    plan->foreign_capacity = INITIAL_MEDIUM_CAPACITY;
    plan->foreign_modules = arena_calloc(plan->foreign_capacity, sizeof(BuildForeignModule));
    plan->package_capacity = INITIAL_MEDIUM_CAPACITY;
    plan->packages = arena_calloc(plan->package_capacity, sizeof(BuildPackage));
    plan->module_capacity = INITIAL_LARGE_CAPACITY;
    plan->modules = arena_calloc(plan->module_capacity, sizeof(BuildModule));
    plan->batch_capacity = INITIAL_SMALL_CAPACITY;
    plan->batches = arena_calloc(plan->batch_capacity, sizeof(BuildBatch));
    plan->problem_capacity = INITIAL_SMALL_CAPACITY;
    plan->problems = arena_calloc(plan->problem_capacity, sizeof(BuildProblem));

    /* Basic fields */
    plan->root = arena_strdup(project_root);
    plan->use_cached = false;  /* Phase 1: always false */

    /* Parse source directories */
    char elm_json_path[MAX_PATH_LENGTH];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", project_root);

    int src_dir_count = 0;
    char **src_dirs = elm_parse_source_directories(elm_json_path, &src_dir_count);
    if (!src_dirs || src_dir_count == 0) {
        /* Default to "src" */
        src_dir_count = 1;
        src_dirs = arena_malloc(sizeof(char*));
        src_dirs[0] = arena_strdup("src");
    }

    /* Store absolute source directory paths */
    plan->src_dir_count = src_dir_count;
    plan->src_dirs = arena_malloc(src_dir_count * sizeof(char*));
    for (int i = 0; i < src_dir_count; i++) {
        char abs_path[MAX_PATH_LENGTH];
        if (src_dirs[i][0] == '/') {
            snprintf(abs_path, sizeof(abs_path), "%s", src_dirs[i]);
        } else {
            snprintf(abs_path, sizeof(abs_path), "%s/%s", project_root, src_dirs[i]);
        }
        plan->src_dirs[i] = arena_strdup(abs_path);
    }

    /* Determine entry point module names */
    plan->root_count = entry_count;
    plan->roots = arena_malloc(entry_count * sizeof(char*));
    for (int i = 0; i < entry_count; i++) {
        /* Convert entry file path to module name */
        for (int s = 0; s < src_dir_count; s++) {
            char *mod_name = path_to_module_name(entry_files[i], plan->src_dirs[s]);
            if (mod_name) {
                plan->roots[i] = mod_name;
                break;
            }
        }
        if (!plan->roots[i]) {
            /* Fallback: extract from filename */
            char *base = arena_strdup(entry_files[i]);
            char *last_slash = strrchr(base, '/');
            if (last_slash) base = last_slash + 1;
            char *dot = strrchr(base, '.');
            if (dot) *dot = '\0';
            /* Replace / with . */
            for (char *p = base; *p; p++) {
                if (*p == '/') *p = '.';
            }
            plan->roots[i] = base;
        }
    }

    /* Step 1: Resolve packages */
    log_debug("Resolving package dependencies...");
    if (!resolve_packages_v2(elm_json, env, plan)) {
        build_add_problem(plan, NULL, "Failed to resolve package dependencies");
        return plan;
    }
    log_debug("Resolved %d packages", plan->package_count);

    /* Step 2: Compute package build order */
    log_debug("Computing package build order...");
    if (!compute_package_build_order(plan, env->cache)) {
        build_add_problem(plan, NULL, "Failed to compute package build order");
        return plan;
    }

    /* Step 2.5: Check artifact status for each package */
    log_debug("Checking package artifact status...");
    check_all_package_artifacts(plan);
    log_debug("Package artifacts: %d present, %d stale, %d missing",
              plan->packages_with_artifacts, plan->packages_stale, plan->packages_missing);

    /* Step 2.6: Build module-to-package mapping for foreign module lookup */
    log_debug("Building module-to-package mapping...");
    build_module_package_map(plan->packages, plan->package_count, env->cache);

    /* Step 3: Crawl modules from entry points (reachability-based discovery) */
    log_debug("Crawling modules from entry points...");
    if (!crawl_modules(project_root, src_dirs, src_dir_count, entry_files, entry_count, plan)) {
        build_add_problem(plan, NULL, "Failed to crawl modules");
        return plan;
    }
    log_debug("Discovered %d local modules, %d foreign modules",
              plan->module_count, plan->foreign_count);

    /* Step 5: Compute module build order */
    log_debug("Computing module build order...");
    if (!compute_module_build_order(plan)) {
        build_add_problem(plan, NULL, "Failed to compute module build order");
        return plan;
    }

    /* Step 6: Compute parallel levels and batches */
    log_debug("Computing parallel batches...");
    compute_parallel_levels(plan);
    group_into_batches(plan);

    /* Summary statistics */
    plan->total_packages = plan->package_count;
    plan->total_modules = plan->module_count;
    plan->modules_to_build = plan->module_count;  /* No cache in Phase 1 */
    plan->parallel_levels = plan->batch_count;

    return plan;
}

void build_plan_free(BuildPlan *plan) {
    /* With arena allocator, this is mostly a no-op */
    (void)plan;
}

/* ============================================================================
 * Package resolution
 * ========================================================================== */

static bool resolve_packages_v2(ElmJson *elm_json, InstallEnv *env,
                                BuildPlan *plan) {
    /*
     * For applications, elm.json already contains the complete dependency tree
     * with exact versions. We can read packages directly without re-solving.
     * This works for both V1 and V2 modes.
     */

    /* Add all direct and indirect dependencies */
    PackageMap *maps[] = {
        elm_json->dependencies_direct,
        elm_json->dependencies_indirect
    };

    for (int m = 0; m < 2; m++) {
        PackageMap *map = maps[m];
        if (!map) continue;

        for (int i = 0; i < map->count; i++) {
            Package *pkg = &map->packages[i];

            DYNARRAY_ENSURE_CAPACITY(plan->packages, plan->package_count,
                                     plan->package_capacity, BuildPackage);

            BuildPackage *bp = &plan->packages[plan->package_count++];
            memset(bp, 0, sizeof(BuildPackage));

            char name_buf[256];
            snprintf(name_buf, sizeof(name_buf), "%s/%s", pkg->author, pkg->name);
            bp->name = arena_strdup(name_buf);
            bp->version = arena_strdup(pkg->version);

            /* Get path to package root and src directory */
            char pkg_path_buf[MAX_PATH_LENGTH];
            snprintf(pkg_path_buf, sizeof(pkg_path_buf), "%s/packages/%s/%s/%s",
                     env->cache->elm_home, pkg->author, pkg->name, pkg->version);
            bp->package_path = arena_strdup(pkg_path_buf);

            char path_buf[MAX_PATH_LENGTH];
            snprintf(path_buf, sizeof(path_buf), "%s/src", pkg_path_buf);
            bp->path = arena_strdup(path_buf);

            bp->dep_capacity = INITIAL_SMALL_CAPACITY;
            bp->deps = arena_calloc(bp->dep_capacity, sizeof(char*));
            bp->artifact_status = ARTIFACT_MISSING;  /* Default, updated later */
        }
    }

    return true;
}

/* ============================================================================
 * Artifact fingerprint parsing and validation
 *
 * artifacts.dat binary format (Haskell Data.Binary encoding):
 *   - 8 bytes: Set size (number of fingerprints)
 *   - For each fingerprint:
 *     - 8 bytes: Map size (number of package entries)
 *     - For each entry:
 *       - 1 byte: author string length
 *       - N bytes: author string
 *       - 1 byte: project string length
 *       - M bytes: project string
 *       - 3 or 7 bytes: version (compact if major < 255, extended otherwise)
 * ========================================================================== */

/* A single package->version entry in a fingerprint */
typedef struct {
    char *name;      /* "author/project" format */
    char *version;   /* "major.minor.patch" format */
} FingerprintEntry;

/* A fingerprint is a set of package versions */
typedef struct {
    FingerprintEntry *entries;
    int count;
} Fingerprint;

/* Read a big-endian 64-bit integer from buffer */
static uint64_t read_be64(const uint8_t *buf) {
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8)  | ((uint64_t)buf[7]);
}

/* Read a big-endian 16-bit integer from buffer */
static uint16_t read_be16(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

/**
 * Parse fingerprints from artifacts.dat binary data.
 * Returns array of Fingerprints, sets *count to number of fingerprints.
 * Returns NULL on parse error.
 */
static Fingerprint *parse_artifact_fingerprints(const uint8_t *data, size_t size,
                                                 int *fingerprint_count) {
    if (size < 8) return NULL;

    size_t pos = 0;

    /* Read set size (number of fingerprints) */
    uint64_t set_size = read_be64(data + pos);
    pos += 8;

    if (set_size > 1000) {
        /* Sanity check - shouldn't have more than 1000 fingerprints */
        return NULL;
    }

    Fingerprint *fingerprints = arena_calloc((int)set_size, sizeof(Fingerprint));
    *fingerprint_count = (int)set_size;

    for (uint64_t fp_idx = 0; fp_idx < set_size; fp_idx++) {
        if (pos + 8 > size) goto parse_error;

        /* Read map size (number of packages in this fingerprint) */
        uint64_t map_size = read_be64(data + pos);
        pos += 8;

        if (map_size > 1000) goto parse_error;

        fingerprints[fp_idx].count = (int)map_size;
        fingerprints[fp_idx].entries = arena_calloc((int)map_size, sizeof(FingerprintEntry));

        for (uint64_t entry_idx = 0; entry_idx < map_size; entry_idx++) {
            if (pos + 1 > size) goto parse_error;

            /* Read author string */
            uint8_t author_len = data[pos++];
            if (pos + author_len > size) goto parse_error;
            char author[256];
            memcpy(author, data + pos, author_len);
            author[author_len] = '\0';
            pos += author_len;

            /* Read project string */
            if (pos + 1 > size) goto parse_error;
            uint8_t project_len = data[pos++];
            if (pos + project_len > size) goto parse_error;
            char project[256];
            memcpy(project, data + pos, project_len);
            project[project_len] = '\0';
            pos += project_len;

            /* Read version */
            if (pos + 1 > size) goto parse_error;
            uint16_t major, minor, patch;
            if (data[pos] == 255) {
                /* Extended format: 255 marker + 3 Word16s */
                pos++;
                if (pos + 6 > size) goto parse_error;
                major = read_be16(data + pos); pos += 2;
                minor = read_be16(data + pos); pos += 2;
                patch = read_be16(data + pos); pos += 2;
            } else {
                /* Compact format: 3 bytes */
                if (pos + 3 > size) goto parse_error;
                major = data[pos++];
                minor = data[pos++];
                patch = data[pos++];
            }

            /* Store entry */
            char name_buf[256];
            snprintf(name_buf, sizeof(name_buf), "%s/%s", author, project);
            fingerprints[fp_idx].entries[entry_idx].name = arena_strdup(name_buf);

            char version_buf[64];
            snprintf(version_buf, sizeof(version_buf), "%u.%u.%u", major, minor, patch);
            fingerprints[fp_idx].entries[entry_idx].version = arena_strdup(version_buf);
        }
    }

    return fingerprints;

parse_error:
    return NULL;
}

/**
 * Build the expected fingerprint for a package based on current project dependencies.
 * The fingerprint contains the versions of all packages that this package depends on.
 */
static Fingerprint build_expected_fingerprint(BuildPackage *pkg, BuildPlan *plan) {
    Fingerprint fp = {0};

    if (pkg->dep_count == 0) {
        return fp;
    }

    fp.entries = arena_calloc(pkg->dep_count, sizeof(FingerprintEntry));
    fp.count = 0;

    for (int i = 0; i < pkg->dep_count; i++) {
        /* Find this dependency in the plan's packages to get its version */
        for (int j = 0; j < plan->package_count; j++) {
            if (strcmp(plan->packages[j].name, pkg->deps[i]) == 0) {
                fp.entries[fp.count].name = plan->packages[j].name;
                fp.entries[fp.count].version = plan->packages[j].version;
                fp.count++;
                break;
            }
        }
    }

    return fp;
}

/**
 * Compare two fingerprints for equality.
 * Returns true if they contain the same packages with the same versions.
 */
static bool fingerprints_match(const Fingerprint *a, const Fingerprint *b) {
    if (a->count != b->count) return false;

    /* For each entry in a, find matching entry in b */
    for (int i = 0; i < a->count; i++) {
        bool found = false;
        for (int j = 0; j < b->count; j++) {
            if (strcmp(a->entries[i].name, b->entries[j].name) == 0 &&
                strcmp(a->entries[i].version, b->entries[j].version) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

/**
 * Check artifact status for a package with fingerprint validation.
 */
static ArtifactStatus check_package_artifact_status(BuildPackage *pkg, BuildPlan *plan) {
    char artifact_path[MAX_PATH_LENGTH];
    snprintf(artifact_path, sizeof(artifact_path), "%s/artifacts.dat", pkg->package_path);

    /* Check if file exists */
    struct stat st;
    if (stat(artifact_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ARTIFACT_MISSING;
    }

    /* Read file contents */
    FILE *f = fopen(artifact_path, "rb");
    if (!f) return ARTIFACT_MISSING;

    size_t file_size = (size_t)st.st_size;
    uint8_t *data = arena_malloc(file_size);
    if (!data) {
        fclose(f);
        return ARTIFACT_MISSING;
    }

    size_t bytes_read = fread(data, 1, file_size, f);
    fclose(f);

    if (bytes_read != file_size) {
        return ARTIFACT_MISSING;
    }

    /* Parse fingerprints from file */
    int fingerprint_count = 0;
    Fingerprint *fingerprints = parse_artifact_fingerprints(data, file_size, &fingerprint_count);

    if (!fingerprints) {
        /* Parse error - treat as stale (file exists but unreadable) */
        return ARTIFACT_STALE;
    }

    /* Build expected fingerprint based on current project dependencies */
    Fingerprint expected = build_expected_fingerprint(pkg, plan);

    /* Check if any stored fingerprint matches the expected one */
    for (int i = 0; i < fingerprint_count; i++) {
        if (fingerprints_match(&fingerprints[i], &expected)) {
            return ARTIFACT_PRESENT;
        }
    }

    /* File exists but no matching fingerprint */
    return ARTIFACT_STALE;
}

/**
 * Check artifact status for all packages and update summary statistics.
 */
static void check_all_package_artifacts(BuildPlan *plan) {
    plan->packages_with_artifacts = 0;
    plan->packages_stale = 0;
    plan->packages_missing = 0;

    for (int i = 0; i < plan->package_count; i++) {
        BuildPackage *pkg = &plan->packages[i];
        pkg->artifact_status = check_package_artifact_status(pkg, plan);

        switch (pkg->artifact_status) {
            case ARTIFACT_PRESENT:
                plan->packages_with_artifacts++;
                break;
            case ARTIFACT_STALE:
                plan->packages_stale++;
                break;
            case ARTIFACT_MISSING:
                plan->packages_missing++;
                break;
        }
    }
}

/* ============================================================================
 * Package topological sort
 * ========================================================================== */

static bool compute_package_build_order(BuildPlan *plan, CacheConfig *cache) {
    if (plan->package_count == 0) return true;

    /* Read each package's elm.json to get its dependencies */
    for (int i = 0; i < plan->package_count; i++) {
        BuildPackage *pkg = &plan->packages[i];

        /* Parse author and name from "author/name" */
        char *slash = strchr(pkg->name, '/');
        if (!slash) continue;

        size_t author_len = slash - pkg->name;
        char author[256];
        strncpy(author, pkg->name, author_len);
        author[author_len] = '\0';
        const char *name = slash + 1;

        /* Read package elm.json */
        char elm_json_path[MAX_PATH_LENGTH];
        snprintf(elm_json_path, sizeof(elm_json_path),
                 "%s/packages/%s/%s/%s/elm.json",
                 cache->elm_home, author, name, pkg->version);

        ElmJson *pkg_ej = elm_json_read(elm_json_path);
        if (!pkg_ej) {
            log_debug("Could not read %s", elm_json_path);
            continue;
        }

        /* Extract dependencies */
        PackageMap *deps = pkg_ej->package_dependencies;
        if (deps) {
            for (int j = 0; j < deps->count; j++) {
                Package *dep = &deps->packages[j];
                char dep_name[256];
                snprintf(dep_name, sizeof(dep_name), "%s/%s", dep->author, dep->name);

                /* Only add if this dep is in our package set */
                bool found = false;
                for (int k = 0; k < plan->package_count; k++) {
                    if (strcmp(plan->packages[k].name, dep_name) == 0) {
                        found = true;
                        break;
                    }
                }

                if (found) {
                    DYNARRAY_ENSURE_CAPACITY(pkg->deps, pkg->dep_count,
                                             pkg->dep_capacity, char*);
                    pkg->deps[pkg->dep_count++] = arena_strdup(dep_name);
                }
            }
        }

        elm_json_free(pkg_ej);
    }

    /* Sort packages alphabetically first for deterministic ordering */
    for (int i = 0; i < plan->package_count - 1; i++) {
        for (int j = i + 1; j < plan->package_count; j++) {
            if (strcmp(plan->packages[i].name, plan->packages[j].name) > 0) {
                BuildPackage tmp = plan->packages[i];
                plan->packages[i] = plan->packages[j];
                plan->packages[j] = tmp;
            }
        }
    }

    /* Kahn's algorithm with stable ordering */
    int *in_degree = arena_calloc(plan->package_count, sizeof(int));
    int *order = arena_malloc(plan->package_count * sizeof(int));
    int order_count = 0;

    /* Compute in-degrees */
    for (int i = 0; i < plan->package_count; i++) {
        BuildPackage *pkg = &plan->packages[i];
        for (int j = 0; j < pkg->dep_count; j++) {
            for (int k = 0; k < plan->package_count; k++) {
                if (strcmp(plan->packages[k].name, pkg->deps[j]) == 0) {
                    in_degree[i]++;
                    break;
                }
            }
        }
    }

    /* Use stable ordering - always pick alphabetically first ready package */
    bool *processed = arena_calloc(plan->package_count, sizeof(bool));

    while (order_count < plan->package_count) {
        int next = -1;
        for (int i = 0; i < plan->package_count; i++) {
            if (!processed[i] && in_degree[i] == 0) {
                next = i;
                break;
            }
        }

        if (next == -1) {
            log_error("Cycle detected in package dependencies");
            return false;
        }

        processed[next] = true;
        order[order_count++] = next;

        BuildPackage *pkg = &plan->packages[next];
        for (int i = 0; i < plan->package_count; i++) {
            if (processed[i]) continue;
            BuildPackage *other = &plan->packages[i];
            for (int j = 0; j < other->dep_count; j++) {
                if (strcmp(other->deps[j], pkg->name) == 0) {
                    in_degree[i]--;
                    break;
                }
            }
        }
    }

    /* Check for cycle */
    if (order_count != plan->package_count) {
        log_error("Cycle detected in package dependencies");
        return false;
    }

    /* Reorder packages array */
    BuildPackage *ordered = arena_malloc(plan->package_count * sizeof(BuildPackage));
    for (int i = 0; i < plan->package_count; i++) {
        ordered[i] = plan->packages[order[i]];
    }
    memcpy(plan->packages, ordered, plan->package_count * sizeof(BuildPackage));

    return true;
}

/* ============================================================================
 * Module discovery - Reachability-based crawling from entry points
 * ========================================================================== */

/* Convert module name to file path by searching source directories */
static char *module_name_to_path(const char *module_name, const char *project_root,
                                  char **src_dirs, int src_dir_count) {
    /* Convert Module.Name to Module/Name.elm */
    char rel_path[MAX_PATH_LENGTH];
    strncpy(rel_path, module_name, sizeof(rel_path) - 5);
    rel_path[sizeof(rel_path) - 5] = '\0';

    /* Replace dots with slashes */
    for (char *p = rel_path; *p; p++) {
        if (*p == '.') *p = '/';
    }
    strcat(rel_path, ".elm");

    /* Search in each source directory */
    for (int d = 0; d < src_dir_count; d++) {
        char full_path[MAX_PATH_LENGTH];
        if (src_dirs[d][0] == '/') {
            snprintf(full_path, sizeof(full_path), "%s/%s", src_dirs[d], rel_path);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s/%s", project_root, src_dirs[d], rel_path);
        }

        /* Check if file exists */
        FILE *f = fopen(full_path, "r");
        if (f) {
            fclose(f);
            return arena_strdup(full_path);
        }
    }

    return NULL;
}

/* Check if a module name is already in the discovered set */
static bool is_module_discovered(const char *name, char **discovered, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(discovered[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Add implicit core imports that Elm always provides */
static void add_implicit_core_imports(BuildPlan *plan) {
    static const char *implicit_imports[] = {
        "Basics",
        "Char",
        "Debug",
        "Maybe",
        "Platform",
        "Platform.Cmd",
        "Platform.Sub",
        "Tuple",
        NULL
    };

    for (int i = 0; implicit_imports[i] != NULL; i++) {
        /* Check if already present */
        bool found = false;
        for (int f = 0; f < plan->foreign_count; f++) {
            if (strcmp(plan->foreign_modules[f].name, implicit_imports[i]) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            DYNARRAY_ENSURE_CAPACITY(plan->foreign_modules, plan->foreign_count,
                                     plan->foreign_capacity, BuildForeignModule);
            BuildForeignModule *fm = &plan->foreign_modules[plan->foreign_count++];
            fm->name = arena_strdup(implicit_imports[i]);
            fm->package = arena_strdup("elm/core");
        }
    }
}

/* Crawl modules starting from entry points, following imports */
static bool crawl_modules(const char *project_root, char **src_dirs, int src_dir_count,
                          const char **entry_files, int entry_count, BuildPlan *plan) {
    /* Work queue for BFS traversal */
    int queue_capacity = INITIAL_LARGE_CAPACITY;
    int queue_count = 0;
    char **queue = arena_malloc(queue_capacity * sizeof(char*));

    /* Track discovered module names */
    int discovered_capacity = INITIAL_LARGE_CAPACITY;
    int discovered_count = 0;
    char **discovered = arena_malloc(discovered_capacity * sizeof(char*));

    /* Add entry points to queue */
    for (int i = 0; i < entry_count; i++) {
        /* Parse entry file to get its module name */
        SkeletonModule *skel = skeleton_parse(entry_files[i]);
        if (!skel) {
            log_error("Failed to parse entry file: %s", entry_files[i]);
            continue;
        }

        const char *mod_name = skel->module_name;
        if (mod_name && !is_module_discovered(mod_name, discovered, discovered_count)) {
            DYNARRAY_ENSURE_CAPACITY(queue, queue_count, queue_capacity, char*);
            queue[queue_count++] = arena_strdup(mod_name);

            DYNARRAY_ENSURE_CAPACITY(discovered, discovered_count, discovered_capacity, char*);
            discovered[discovered_count++] = arena_strdup(mod_name);
        }

        skeleton_free(skel);
    }

    /* BFS: process queue, adding new local imports */
    int queue_front = 0;
    while (queue_front < queue_count) {
        const char *mod_name = queue[queue_front++];

        /* Find file path for this module */
        char *file_path = module_name_to_path(mod_name, project_root, src_dirs, src_dir_count);
        if (!file_path) {
            /* Not a local module - skip */
            continue;
        }

        /* Parse the file */
        SkeletonModule *skel = skeleton_parse(file_path);
        if (!skel) {
            log_debug("Failed to parse %s", file_path);
            continue;
        }

        /* Add module to plan */
        DYNARRAY_ENSURE_CAPACITY(plan->modules, plan->module_count,
                                 plan->module_capacity, BuildModule);

        BuildModule *mod = &plan->modules[plan->module_count++];
        memset(mod, 0, sizeof(BuildModule));

        mod->name = arena_strdup(mod_name);
        mod->path = make_relative_path(file_path, project_root);
        mod->cached = false;
        mod->level = -1;
        mod->has_main = skeleton_find_type_annotation(skel, "main") != NULL;
        mod->dep_capacity = INITIAL_SMALL_CAPACITY;
        mod->deps = arena_calloc(mod->dep_capacity, sizeof(char*));

        /* Process imports */
        for (int j = 0; j < skel->imports_count; j++) {
            const char *import_name = skel->imports[j].module_name;

            /* Check if this is a local module */
            char *import_path = module_name_to_path(import_name, project_root, src_dirs, src_dir_count);

            if (import_path) {
                /* Local module - add as dependency */
                DYNARRAY_ENSURE_CAPACITY(mod->deps, mod->dep_count,
                                         mod->dep_capacity, char*);
                mod->deps[mod->dep_count++] = arena_strdup(import_name);

                /* Add to queue if not already discovered */
                if (!is_module_discovered(import_name, discovered, discovered_count)) {
                    DYNARRAY_ENSURE_CAPACITY(queue, queue_count, queue_capacity, char*);
                    queue[queue_count++] = arena_strdup(import_name);

                    DYNARRAY_ENSURE_CAPACITY(discovered, discovered_count, discovered_capacity, char*);
                    discovered[discovered_count++] = arena_strdup(import_name);
                }
            } else {
                /* Foreign module - add to foreign list */
                bool found = false;
                for (int f = 0; f < plan->foreign_count; f++) {
                    if (strcmp(plan->foreign_modules[f].name, import_name) == 0) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    const char *pkg_name = find_package_for_module(
                        import_name, plan->packages, plan->package_count, NULL);

                    DYNARRAY_ENSURE_CAPACITY(plan->foreign_modules, plan->foreign_count,
                                             plan->foreign_capacity, BuildForeignModule);

                    BuildForeignModule *fm = &plan->foreign_modules[plan->foreign_count++];
                    fm->name = arena_strdup(import_name);
                    fm->package = pkg_name ? arena_strdup(pkg_name) : arena_strdup("unknown");
                }
            }
        }

        skeleton_free(skel);
    }

    /* Add implicit core imports */
    add_implicit_core_imports(plan);

    return true;
}

/* ============================================================================
 * Module topological sort
 * ========================================================================== */

/* Compare strings for qsort */
static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* DFS visit for topological sort - explores deps in alphabetical order */
static void dfs_topo_visit(BuildPlan *plan, int idx, int *visited,
                           int *order, int *order_pos) {
    if (visited[idx]) return;
    visited[idx] = 1;

    BuildModule *mod = &plan->modules[idx];

    /* Sort deps in alphabetical order */
    if (mod->dep_count > 1) {
        qsort(mod->deps, mod->dep_count, sizeof(char*), compare_strings);
    }

    /* Visit deps in alphabetical order */
    for (int d = 0; d < mod->dep_count; d++) {
        int dep_idx = find_module_index(plan->modules, plan->module_count, mod->deps[d]);
        if (dep_idx >= 0 && !visited[dep_idx]) {
            dfs_topo_visit(plan, dep_idx, visited, order, order_pos);
        }
    }

    /* Post-order: add after all deps are done */
    order[(*order_pos)++] = idx;
}

static bool compute_module_build_order(BuildPlan *plan) {
    if (plan->module_count == 0) return true;

    /*
     * DFS post-order with alphabetically sorted adjacency lists.
     * This matches Haskell's Data.Graph.stronglyConnComp behavior:
     * - Modules are visited in alphabetical order
     * - Dependencies are explored in alphabetical order
     * - Output is post-order (deps finish before dependents)
     */

    /* Sort modules alphabetically by name (for consistent lookups) */
    for (int i = 0; i < plan->module_count - 1; i++) {
        for (int j = i + 1; j < plan->module_count; j++) {
            if (strcmp(plan->modules[i].name, plan->modules[j].name) > 0) {
                BuildModule tmp = plan->modules[i];
                plan->modules[i] = plan->modules[j];
                plan->modules[j] = tmp;
            }
        }
    }

    int *visited = arena_calloc(plan->module_count, sizeof(int));
    int *order = arena_malloc(plan->module_count * sizeof(int));
    int order_pos = 0;

    /* Visit modules in alphabetical order (A to Z) */
    for (int i = 0; i < plan->module_count; i++) {
        if (!visited[i]) {
            dfs_topo_visit(plan, i, visited, order, &order_pos);
        }
    }

    if (order_pos != plan->module_count) {
        log_error("Cycle detected in module dependencies");
        return false;
    }

    /* Reorder modules array */
    BuildModule *ordered = arena_malloc(plan->module_count * sizeof(BuildModule));
    for (int i = 0; i < plan->module_count; i++) {
        ordered[i] = plan->modules[order[i]];
    }
    memcpy(plan->modules, ordered, plan->module_count * sizeof(BuildModule));

    return true;
}

/* ============================================================================
 * Parallel batch computation
 * ========================================================================== */

static void compute_parallel_levels(BuildPlan *plan) {
    /* Initialize all levels to -1 */
    for (int i = 0; i < plan->module_count; i++) {
        plan->modules[i].level = -1;
    }

    /* Fixed-point iteration */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < plan->module_count; i++) {
            BuildModule *mod = &plan->modules[i];

            if (mod->dep_count == 0) {
                if (mod->level != 0) {
                    mod->level = 0;
                    changed = true;
                }
            } else {
                /* Check if all deps have levels computed */
                bool all_ready = true;
                int max_dep_level = -1;

                for (int j = 0; j < mod->dep_count; j++) {
                    int dep_idx = find_module_index(plan->modules, plan->module_count, mod->deps[j]);
                    if (dep_idx < 0 || plan->modules[dep_idx].level == -1) {
                        all_ready = false;
                        break;
                    }
                    if (plan->modules[dep_idx].level > max_dep_level) {
                        max_dep_level = plan->modules[dep_idx].level;
                    }
                }

                if (all_ready) {
                    int new_level = max_dep_level + 1;
                    if (mod->level != new_level) {
                        mod->level = new_level;
                        changed = true;
                    }
                }
            }
        }
    }
}

static void group_into_batches(BuildPlan *plan) {
    /* Find max level */
    int max_level = 0;
    for (int i = 0; i < plan->module_count; i++) {
        if (plan->modules[i].level > max_level) {
            max_level = plan->modules[i].level;
        }
    }

    plan->batch_count = max_level + 1;
    if (plan->batch_count > plan->batch_capacity) {
        plan->batch_capacity = plan->batch_count;
        plan->batches = arena_realloc(plan->batches,
                                       plan->batch_capacity * sizeof(BuildBatch));
    }

    /* Initialize batches */
    for (int l = 0; l <= max_level; l++) {
        plan->batches[l].level = l;
        plan->batches[l].count = 0;
        plan->batches[l].modules_capacity = INITIAL_SMALL_CAPACITY;
        plan->batches[l].modules = arena_malloc(
            plan->batches[l].modules_capacity * sizeof(BuildModule*));
    }

    /* Group modules by level */
    for (int i = 0; i < plan->module_count; i++) {
        int level = plan->modules[i].level;
        if (level < 0 || level > max_level) continue;

        BuildBatch *batch = &plan->batches[level];
        DYNARRAY_ENSURE_CAPACITY(batch->modules, batch->count,
                                 batch->modules_capacity, BuildModule*);
        batch->modules[batch->count++] = &plan->modules[i];
    }
}

/* ============================================================================
 * Helper functions
 * ========================================================================== */

static char *path_to_module_name(const char *file_path, const char *src_dir) {
    /* Check if file_path starts with src_dir */
    size_t src_len = strlen(src_dir);
    if (strncmp(file_path, src_dir, src_len) != 0) {
        return NULL;
    }

    /* Skip src_dir and leading slash */
    const char *relative = file_path + src_len;
    if (*relative == '/') relative++;

    /* Must end with .elm */
    size_t rel_len = strlen(relative);
    if (rel_len < 4 || strcmp(relative + rel_len - 4, ".elm") != 0) {
        return NULL;
    }

    /* Copy and convert slashes to dots */
    char *result = arena_malloc(rel_len);
    strncpy(result, relative, rel_len - 4);
    result[rel_len - 4] = '\0';

    for (char *p = result; *p; p++) {
        if (*p == '/') *p = '.';
    }

    return result;
}

static char *make_relative_path(const char *abs_path, const char *base_path) {
    size_t base_len = strlen(base_path);

    if (strncmp(abs_path, base_path, base_len) == 0) {
        const char *rel = abs_path + base_len;
        if (*rel == '/') rel++;
        return arena_strdup(rel);
    }

    return arena_strdup(abs_path);
}

static int find_module_index(BuildModule *modules, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Module-to-package mapping */
typedef struct {
    char *module_name;
    char *package_name;
} ModulePackageMapping;

static ModulePackageMapping *g_module_map = NULL;
static int g_module_map_count = 0;
static int g_module_map_capacity = 0;

static void add_module_mapping(const char *module_name, const char *package_name) {
    if (g_module_map_count >= g_module_map_capacity) {
        g_module_map_capacity *= 2;
        g_module_map = arena_realloc(g_module_map,
            g_module_map_capacity * sizeof(ModulePackageMapping));
    }
    g_module_map[g_module_map_count].module_name = arena_strdup(module_name);
    g_module_map[g_module_map_count].package_name = arena_strdup(package_name);
    g_module_map_count++;
}

static void parse_exposed_modules_array(cJSON *arr, const char *pkg_name) {
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item)) {
            add_module_mapping(item->valuestring, pkg_name);
        }
    }
}

static void build_module_package_map(BuildPackage *packages, int pkg_count, CacheConfig *cache) {
    g_module_map_capacity = 1024;
    g_module_map_count = 0;
    g_module_map = arena_malloc(g_module_map_capacity * sizeof(ModulePackageMapping));

    for (int i = 0; i < pkg_count; i++) {
        BuildPackage *pkg = &packages[i];

        /* Parse author/name from package name */
        char *slash = strchr(pkg->name, '/');
        if (!slash) continue;

        size_t author_len = slash - pkg->name;
        char author[256];
        strncpy(author, pkg->name, author_len);
        author[author_len] = '\0';
        const char *name = slash + 1;

        /* Read package elm.json directly with cJSON */
        char elm_json_path[MAX_PATH_LENGTH];
        snprintf(elm_json_path, sizeof(elm_json_path),
                 "%s/packages/%s/%s/%s/elm.json",
                 cache->elm_home, author, name, pkg->version);

        FILE *f = fopen(elm_json_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *content = arena_malloc(fsize + 1);
        size_t read_size = fread(content, 1, fsize, f);
        fclose(f);
        content[read_size] = '\0';

        cJSON *root = cJSON_Parse(content);
        if (!root) continue;

        /* Get exposed-modules - can be array or object with categories */
        cJSON *exposed = cJSON_GetObjectItem(root, "exposed-modules");
        if (exposed) {
            if (cJSON_IsArray(exposed)) {
                parse_exposed_modules_array(exposed, pkg->name);
            } else if (cJSON_IsObject(exposed)) {
                /* Object with categories like {"A": [...], "B": [...]} */
                cJSON *category;
                cJSON_ArrayForEach(category, exposed) {
                    if (cJSON_IsArray(category)) {
                        parse_exposed_modules_array(category, pkg->name);
                    }
                }
            }
        }

        cJSON_Delete(root);
    }
}

static const char *find_package_for_module(const char *module_name,
                                           BuildPackage *packages, int pkg_count,
                                           CacheConfig *cache) {
    (void)packages;
    (void)pkg_count;
    (void)cache;

    /* Look up in pre-built map */
    for (int i = 0; i < g_module_map_count; i++) {
        if (strcmp(g_module_map[i].module_name, module_name) == 0) {
            return g_module_map[i].package_name;
        }
    }

    /* Fallback to common prefixes for core modules */
    char prefix[256];
    strncpy(prefix, module_name, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';

    /* Get first component */
    char *dot = strchr(prefix, '.');
    if (dot) *dot = '\0';

    /* Common mappings */
    static const struct {
        const char *module_prefix;
        const char *package;
    } common_mappings[] = {
        {"Array", "elm/core"},
        {"Basics", "elm/core"},
        {"Bitwise", "elm/core"},
        {"Char", "elm/core"},
        {"Debug", "elm/core"},
        {"Dict", "elm/core"},
        {"List", "elm/core"},
        {"Maybe", "elm/core"},
        {"Platform", "elm/core"},
        {"Process", "elm/core"},
        {"Result", "elm/core"},
        {"Set", "elm/core"},
        {"String", "elm/core"},
        {"Task", "elm/core"},
        {"Tuple", "elm/core"},
        {"Json", "elm/json"},
        {"Html", "elm/html"},
        {"Svg", "elm/svg"},
        {"Browser", "elm/browser"},
        {"Http", "elm/http"},
        {"Url", "elm/url"},
        {"File", "elm/file"},
        {"Bytes", "elm/bytes"},
        {"Parser", "elm/parser"},
        {"Regex", "elm/regex"},
        {"Time", "elm/time"},
        {"Random", "elm/random"},
        {NULL, NULL}
    };

    for (int i = 0; common_mappings[i].module_prefix != NULL; i++) {
        if (strcmp(prefix, common_mappings[i].module_prefix) == 0) {
            return common_mappings[i].package;
        }
    }

    /* Search in our package list */
    for (int i = 0; i < pkg_count; i++) {
        /* Check if package name contains the prefix */
        if (strstr(packages[i].name, prefix)) {
            return packages[i].name;
        }
    }

    return NULL;
}

/* ============================================================================
 * JSON output
 * ========================================================================== */

char *build_plan_to_json(BuildPlan *plan) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    /* Basic fields */
    cJSON_AddStringToObject(root, "root", plan->root);

    /* srcDirs */
    cJSON *src_dirs = cJSON_CreateArray();
    for (int i = 0; i < plan->src_dir_count; i++) {
        cJSON_AddItemToArray(src_dirs, cJSON_CreateString(plan->src_dirs[i]));
    }
    cJSON_AddItemToObject(root, "srcDirs", src_dirs);

    cJSON_AddBoolToObject(root, "useCached", plan->use_cached);

    /* roots */
    cJSON *roots = cJSON_CreateArray();
    for (int i = 0; i < plan->root_count; i++) {
        cJSON_AddItemToArray(roots, cJSON_CreateString(plan->roots[i]));
    }
    cJSON_AddItemToObject(root, "roots", roots);

    /* foreignModules */
    cJSON *foreign = cJSON_CreateArray();
    for (int i = 0; i < plan->foreign_count; i++) {
        cJSON *fm = cJSON_CreateObject();
        cJSON_AddStringToObject(fm, "name", plan->foreign_modules[i].name);
        cJSON_AddStringToObject(fm, "package", plan->foreign_modules[i].package);
        cJSON_AddItemToArray(foreign, fm);
    }
    cJSON_AddItemToObject(root, "foreignModules", foreign);

    /* packageBuildOrder */
    cJSON *pkg_order = cJSON_CreateArray();
    for (int i = 0; i < plan->package_count; i++) {
        BuildPackage *pkg = &plan->packages[i];
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", pkg->name);
        cJSON_AddStringToObject(p, "version", pkg->version);
        cJSON_AddStringToObject(p, "path", pkg->path);

        cJSON *deps = cJSON_CreateArray();
        for (int j = 0; j < pkg->dep_count; j++) {
            cJSON_AddItemToArray(deps, cJSON_CreateString(pkg->deps[j]));
        }
        cJSON_AddItemToObject(p, "deps", deps);

        /* Artifact status */
        const char *status_str;
        switch (pkg->artifact_status) {
            case ARTIFACT_PRESENT: status_str = "present"; break;
            case ARTIFACT_STALE:   status_str = "stale"; break;
            case ARTIFACT_MISSING: status_str = "missing"; break;
            default:               status_str = "unknown"; break;
        }
        cJSON_AddStringToObject(p, "artifactStatus", status_str);

        cJSON_AddItemToArray(pkg_order, p);
    }
    cJSON_AddItemToObject(root, "packageBuildOrder", pkg_order);

    /* buildOrder */
    cJSON *build_order = cJSON_CreateArray();
    for (int i = 0; i < plan->module_count; i++) {
        BuildModule *mod = &plan->modules[i];
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "name", mod->name);
        cJSON_AddStringToObject(m, "path", mod->path);

        cJSON *deps = cJSON_CreateArray();
        for (int j = 0; j < mod->dep_count; j++) {
            cJSON_AddItemToArray(deps, cJSON_CreateString(mod->deps[j]));
        }
        cJSON_AddItemToObject(m, "deps", deps);

        cJSON_AddBoolToObject(m, "hasMain", mod->has_main);
        cJSON_AddBoolToObject(m, "cached", mod->cached);

        cJSON_AddItemToArray(build_order, m);
    }
    cJSON_AddItemToObject(root, "buildOrder", build_order);

    /* parallelBatches */
    cJSON *batches = cJSON_CreateArray();
    for (int i = 0; i < plan->batch_count; i++) {
        BuildBatch *batch = &plan->batches[i];
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "level", batch->level);
        cJSON_AddNumberToObject(b, "count", batch->count);

        cJSON *modules = cJSON_CreateArray();
        for (int j = 0; j < batch->count; j++) {
            BuildModule *mod = batch->modules[j];
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "name", mod->name);
            cJSON_AddStringToObject(m, "path", mod->path);

            cJSON *deps = cJSON_CreateArray();
            for (int k = 0; k < mod->dep_count; k++) {
                cJSON_AddItemToArray(deps, cJSON_CreateString(mod->deps[k]));
            }
            cJSON_AddItemToObject(m, "deps", deps);

            cJSON_AddBoolToObject(m, "hasMain", mod->has_main);
            cJSON_AddBoolToObject(m, "cached", mod->cached);

            cJSON_AddItemToArray(modules, m);
        }
        cJSON_AddItemToObject(b, "modules", modules);

        cJSON_AddItemToArray(batches, b);
    }
    cJSON_AddItemToObject(root, "parallelBatches", batches);

    /* problems */
    cJSON *problems = cJSON_CreateArray();
    for (int i = 0; i < plan->problem_count; i++) {
        cJSON *p = cJSON_CreateObject();
        if (plan->problems[i].module_name) {
            cJSON_AddStringToObject(p, "module", plan->problems[i].module_name);
        }
        cJSON_AddStringToObject(p, "message", plan->problems[i].message);
        cJSON_AddItemToArray(problems, p);
    }
    cJSON_AddItemToObject(root, "problems", problems);

    /* Summary statistics */
    cJSON_AddNumberToObject(root, "totalPackages", plan->total_packages);
    cJSON_AddNumberToObject(root, "totalModules", plan->total_modules);
    cJSON_AddNumberToObject(root, "modulesToBuild", plan->modules_to_build);
    cJSON_AddNumberToObject(root, "parallelLevels", plan->parallel_levels);
    cJSON_AddNumberToObject(root, "packagesWithArtifacts", plan->packages_with_artifacts);
    cJSON_AddNumberToObject(root, "packagesStale", plan->packages_stale);
    cJSON_AddNumberToObject(root, "packagesMissing", plan->packages_missing);

    /* Generate string */
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    /* cJSON uses arena allocator in this codebase, so just return directly */
    return json_str;
}
