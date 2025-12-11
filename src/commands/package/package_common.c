#include "package_common.h"
#include "../../alloc.h"
#include "../../cache.h"
#include "../../constants.h"
#include "../../fileutil.h"
#include "../../install_env.h"
#include "../../registry.h"
#include "../../solver.h"
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

/* ========================================================================
 * Version Parsing and Formatting
 * ======================================================================== */

Version version_parse(const char *version_str) {
    Version v = {0, 0, 0};
    if (!version_str) {
        return v;
    }

    int major = 0;
    int minor = 0;
    int patch = 0;

    if (sscanf(version_str, "%d.%d.%d", &major, &minor, &patch) == 3 &&
        major >= 0 && minor >= 0 && patch >= 0) {
        v.major = (uint16_t)major;
        v.minor = (uint16_t)minor;
        v.patch = (uint16_t)patch;
    }

    return v;
}

bool version_parse_safe(const char *version_str, Version *out) {
    if (!version_str || !out) {
        return false;
    }

    int major = 0;
    int minor = 0;
    int patch = 0;

    if (sscanf(version_str, "%d.%d.%d", &major, &minor, &patch) != 3) {
        return false;
    }

    if (major < 0 || minor < 0 || patch < 0) {
        return false;
    }

    out->major = (uint16_t)major;
    out->minor = (uint16_t)minor;
    out->patch = (uint16_t)patch;
    return true;
}

char *version_to_string(const Version *v) {
    if (!v) {
        return NULL;
    }

    char *str = arena_malloc(MAX_VERSION_STRING_LENGTH);
    if (!str) {
        return NULL;
    }

    snprintf(str, MAX_VERSION_STRING_LENGTH, "%u.%u.%u",
             (unsigned int)v->major,
             (unsigned int)v->minor,
             (unsigned int)v->patch);
    return str;
}

char *version_format(uint16_t major, uint16_t minor, uint16_t patch) {
    Version v = {major, minor, patch};
    return version_to_string(&v);
}

/* ========================================================================
 * Version Comparison
 * ======================================================================== */

int version_compare(const Version *a, const Version *b) {
    if (!a || !b) {
        return 0;
    }

    if (a->major != b->major) {
        return (int)a->major - (int)b->major;
    }

    if (a->minor != b->minor) {
        return (int)a->minor - (int)b->minor;
    }

    return (int)a->patch - (int)b->patch;
}

bool version_equals(const Version *a, const Version *b) {
    if (!a || !b) {
        return false;
    }

    return (a->major == b->major &&
            a->minor == b->minor &&
            a->patch == b->patch);
}

/* ========================================================================
 * Version Constraints
 * ======================================================================== */

bool version_is_constraint(const char *version_str) {
    if (!version_str) {
        return false;
    }

    return strstr(version_str, "<=") != NULL || strstr(version_str, "<") != NULL;
}

bool version_parse_constraint(const char *constraint, VersionRange *out) {
    if (!constraint || !out) {
        return false;
    }

    int lower_major = 0;
    int lower_minor = 0;
    int lower_patch = 0;
    int upper_major = 0;
    int upper_minor = 0;
    int upper_patch = 0;

    int matched = sscanf(
        constraint,
        " %d.%d.%d <= v < %d.%d.%d",
        &lower_major,
        &lower_minor,
        &lower_patch,
        &upper_major,
        &upper_minor,
        &upper_patch
    );

    if (matched == 6) {
        if (lower_major < 0 || lower_minor < 0 || lower_patch < 0 ||
            upper_major < 0 || upper_minor < 0 || upper_patch < 0) {
            return false;
        }

        out->lower.v.major = (uint16_t)lower_major;
        out->lower.v.minor = (uint16_t)lower_minor;
        out->lower.v.patch = (uint16_t)lower_patch;
        out->lower.inclusive = true;
        out->lower.unbounded = false;

        out->upper.v.major = (uint16_t)upper_major;
        out->upper.v.minor = (uint16_t)upper_minor;
        out->upper.v.patch = (uint16_t)upper_patch;
        out->upper.inclusive = false;
        out->upper.unbounded = false;

        out->is_empty = false;
        return true;
    }

    matched = sscanf(constraint, "%d.%d.%d", &lower_major, &lower_minor, &lower_patch);
    if (matched == 3 &&
        lower_major >= 0 && lower_minor >= 0 && lower_patch >= 0) {
        Version v = {
            (uint16_t)lower_major,
            (uint16_t)lower_minor,
            (uint16_t)lower_patch
        };
        *out = version_range_exact(v);
        return true;
    }

    return false;
}

bool version_in_range(const Version *v, const VersionRange *range) {
    if (!v || !range || range->is_empty) {
        return false;
    }

    if (!range->lower.unbounded) {
        int cmp = version_compare(v, &range->lower.v);
        if (range->lower.inclusive) {
            if (cmp < 0) {
                return false;
            }
        } else if (cmp <= 0) {
            return false;
        }
    }

    if (!range->upper.unbounded) {
        int cmp = version_compare(v, &range->upper.v);
        if (range->upper.inclusive) {
            if (cmp > 0) {
                return false;
            }
        } else if (cmp >= 0) {
            return false;
        }
    }

    return true;
}

char *version_range_to_string(const VersionRange *range) {
    if (!range) {
        return NULL;
    }

    char *out = arena_malloc(MAX_RANGE_STRING_LENGTH);
    if (!out) {
        return NULL;
    }

    if (range->is_empty) {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "(empty)");
        return out;
    }

    if (range->lower.unbounded && range->upper.unbounded) {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "any");
        return out;
    }

    if (!range->lower.unbounded &&
        !range->upper.unbounded &&
        range->lower.inclusive &&
        range->upper.inclusive &&
        version_equals(&range->lower.v, &range->upper.v)) {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "%u.%u.%u",
                 (unsigned int)range->lower.v.major,
                 (unsigned int)range->lower.v.minor,
                 (unsigned int)range->lower.v.patch);
        return out;
    }

    if (!range->lower.unbounded &&
        !range->upper.unbounded &&
        range->lower.inclusive &&
        !range->upper.inclusive &&
        range->upper.v.minor == 0 &&
        range->upper.v.patch == 0 &&
        range->upper.v.major == (uint16_t)(range->lower.v.major + 1)) {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "^%u.%u.%u",
                 (unsigned int)range->lower.v.major,
                 (unsigned int)range->lower.v.minor,
                 (unsigned int)range->lower.v.patch);
        return out;
    }

    char lower_str[MAX_VERSION_STRING_MEDIUM_LENGTH] = "";
    char upper_str[MAX_VERSION_STRING_MEDIUM_LENGTH] = "";

    if (!range->lower.unbounded) {
        snprintf(lower_str, sizeof(lower_str), "%s%u.%u.%u",
                 range->lower.inclusive ? ">=" : ">",
                 (unsigned int)range->lower.v.major,
                 (unsigned int)range->lower.v.minor,
                 (unsigned int)range->lower.v.patch);
    }

    if (!range->upper.unbounded) {
        snprintf(upper_str, sizeof(upper_str), "%s%u.%u.%u",
                 range->upper.inclusive ? "<=" : "<",
                 (unsigned int)range->upper.v.major,
                 (unsigned int)range->upper.v.minor,
                 (unsigned int)range->upper.v.patch);
    }

    if (lower_str[0] != '\0' && upper_str[0] != '\0') {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "%s %s", lower_str, upper_str);
    } else if (lower_str[0] != '\0') {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "%s", lower_str);
    } else if (upper_str[0] != '\0') {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "%s", upper_str);
    } else {
        snprintf(out, MAX_RANGE_STRING_LENGTH, "any");
    }

    return out;
}

/* ========================================================================
 * Package specification parsing
 * ======================================================================== */

bool parse_package_with_version(const char *spec, char **out_author, char **out_name, Version *out_version) {
    if (!spec || !out_author || !out_name || !out_version) {
        return false;
    }

    const char *at_pos = strchr(spec, '@');
    if (!at_pos) {
        return false;
    }

    size_t pkg_len = (size_t)(at_pos - spec);
    char *pkg_part = arena_malloc(pkg_len + 1);
    if (!pkg_part) {
        return false;
    }

    strncpy(pkg_part, spec, pkg_len);
    pkg_part[pkg_len] = '\0';

    if (!parse_package_name(pkg_part, out_author, out_name)) {
        arena_free(pkg_part);
        return false;
    }

    arena_free(pkg_part);

    if (!version_parse_safe(at_pos + 1, out_version)) {
        arena_free(*out_author);
        arena_free(*out_name);
        *out_author = NULL;
        *out_name = NULL;
        return false;
    }

    return true;
}

bool parse_package_spec(const char *spec, char **out_author, char **out_name, char **out_version) {
    if (!spec || !out_author || !out_name || !out_version) {
        return false;
    }

    const char *at_pos = strchr(spec, '@');
    if (!at_pos) {
        return false;
    }

    size_t pkg_len = (size_t)(at_pos - spec);
    char *pkg_part = arena_malloc(pkg_len + 1);
    if (!pkg_part) {
        return false;
    }

    strncpy(pkg_part, spec, pkg_len);
    pkg_part[pkg_len] = '\0';

    if (!parse_package_name(pkg_part, out_author, out_name)) {
        arena_free(pkg_part);
        return false;
    }

    arena_free(pkg_part);

    *out_version = arena_strdup(at_pos + 1);
    if (!*out_version) {
        arena_free(*out_author);
        arena_free(*out_name);
        *out_author = NULL;
        *out_name = NULL;
        return false;
    }

    return true;
}

/* ========================================================================
 * Constraint utilities
 * ======================================================================== */

char *version_to_major_constraint(const char *version) {
    Version v;
    if (!version_parse_safe(version, &v)) {
        return NULL;
    }

    char *constraint = arena_malloc(MAX_RANGE_STRING_LENGTH);
    if (!constraint) {
        return NULL;
    }

    snprintf(constraint, MAX_RANGE_STRING_LENGTH, "%u.%u.%u <= v < %u.0.0",
             (unsigned int)v.major,
             (unsigned int)v.minor,
             (unsigned int)v.patch,
             (unsigned int)(v.major + 1));
    return constraint;
}

VersionRange version_range_exact(Version v) {
    VersionRange range;
    range.lower.v = v;
    range.lower.inclusive = true;
    range.lower.unbounded = false;
    range.upper.v = v;
    range.upper.inclusive = true;
    range.upper.unbounded = false;
    range.is_empty = false;
    return range;
}

VersionRange version_range_until_next_major(Version v) {
    VersionRange range;
    range.lower.v = v;
    range.lower.inclusive = true;
    range.lower.unbounded = false;
    range.upper.v.major = (uint16_t)(v.major + 1);
    range.upper.v.minor = 0;
    range.upper.v.patch = 0;
    range.upper.inclusive = false;
    range.upper.unbounded = false;
    range.is_empty = false;
    return range;
}

VersionRange version_range_until_next_minor(Version v) {
    VersionRange range;
    range.lower.v = v;
    range.lower.inclusive = true;
    range.lower.unbounded = false;
    range.upper.v.major = v.major;
    range.upper.v.minor = (uint16_t)(v.minor + 1);
    range.upper.v.patch = 0;
    range.upper.inclusive = false;
    range.upper.unbounded = false;
    range.is_empty = false;
    return range;
}

VersionRange version_range_any(void) {
    VersionRange range;
    range.lower.v.major = 0;
    range.lower.v.minor = 0;
    range.lower.v.patch = 0;
    range.lower.inclusive = false;
    range.lower.unbounded = true;
    range.upper.v.major = 0;
    range.upper.v.minor = 0;
    range.upper.v.patch = 0;
    range.upper.inclusive = false;
    range.upper.unbounded = true;
    range.is_empty = false;
    return range;
}

static VersionRange version_range_empty(void) {
    VersionRange range = {0};
    range.lower.unbounded = true;
    range.upper.unbounded = true;
    range.is_empty = true;
    return range;
}

VersionRange version_range_intersect(VersionRange a, VersionRange b) {
    if (a.is_empty || b.is_empty) {
        return version_range_empty();
    }

    VersionRange result = version_range_any();
    result.is_empty = false;

    if (a.lower.unbounded && b.lower.unbounded) {
        result.lower.unbounded = true;
        result.lower.inclusive = false;
    } else if (a.lower.unbounded) {
        result.lower = b.lower;
    } else if (b.lower.unbounded) {
        result.lower = a.lower;
    } else {
        int cmp = version_compare(&a.lower.v, &b.lower.v);
        if (cmp > 0) {
            result.lower = a.lower;
        } else if (cmp < 0) {
            result.lower = b.lower;
        } else {
            result.lower.v = a.lower.v;
            result.lower.inclusive = a.lower.inclusive && b.lower.inclusive;
            result.lower.unbounded = false;
        }
    }

    if (a.upper.unbounded && b.upper.unbounded) {
        result.upper.unbounded = true;
        result.upper.inclusive = false;
    } else if (a.upper.unbounded) {
        result.upper = b.upper;
    } else if (b.upper.unbounded) {
        result.upper = a.upper;
    } else {
        int cmp = version_compare(&a.upper.v, &b.upper.v);
        if (cmp < 0) {
            result.upper = a.upper;
        } else if (cmp > 0) {
            result.upper = b.upper;
        } else {
            result.upper.v = a.upper.v;
            result.upper.inclusive = a.upper.inclusive && b.upper.inclusive;
            result.upper.unbounded = false;
        }
    }

    if (!result.lower.unbounded && !result.upper.unbounded) {
        int cmp = version_compare(&result.lower.v, &result.upper.v);
        if (cmp > 0) {
            return version_range_empty();
        }

        if (cmp == 0 && (!result.lower.inclusive || !result.upper.inclusive)) {
            return version_range_empty();
        }
    }

    return result;
}

/* ========================================================================
 * Existing functionality
 * ======================================================================== */

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

PackageMap* find_package_map(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || !author || !name) {
        return NULL;
    }

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        if (package_map_find(elm_json->dependencies_direct, author, name)) {
            return elm_json->dependencies_direct;
        }
        if (package_map_find(elm_json->dependencies_indirect, author, name)) {
            return elm_json->dependencies_indirect;
        }
        if (package_map_find(elm_json->dependencies_test_direct, author, name)) {
            return elm_json->dependencies_test_direct;
        }
        if (package_map_find(elm_json->dependencies_test_indirect, author, name)) {
            return elm_json->dependencies_test_indirect;
        }
    } else {
        if (package_map_find(elm_json->package_dependencies, author, name)) {
            return elm_json->package_dependencies;
        }
        if (package_map_find(elm_json->package_test_dependencies, author, name)) {
            return elm_json->package_test_dependencies;
        }
    }

    return NULL;
}

void remove_from_all_app_maps(ElmJson *elm_json, const char *author, const char *name) {
    if (!elm_json || elm_json->type != ELM_PROJECT_APPLICATION) {
        return;
    }

    package_map_remove(elm_json->dependencies_direct, author, name);
    package_map_remove(elm_json->dependencies_indirect, author, name);
    package_map_remove(elm_json->dependencies_test_direct, author, name);
    package_map_remove(elm_json->dependencies_test_indirect, author, name);
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

bool add_or_update_package_in_elm_json(
    ElmJson *elm_json,
    const char *author,
    const char *name,
    const char *version,
    bool is_test,
    bool is_direct,
    bool remove_first
) {
    if (!elm_json || !author || !name || !version) {
        return false;
    }

    /* Find existing package */
    Package *existing = find_existing_package(elm_json, author, name);

    /* Determine version to use */
    const char *version_to_add = version;
    char *constraint = NULL;

    /* For packages, convert point version to constraint */
    if (elm_json->type == ELM_PROJECT_PACKAGE) {
        constraint = version_to_constraint(version);
        if (constraint) {
            version_to_add = constraint;
        }
    }

    /* Handle update vs add */
    if (existing) {
        /* Update existing entry */
        arena_free(existing->version);
        existing->version = arena_strdup(version_to_add);
        if (constraint) {
            arena_free(constraint);
        }
        return existing->version != NULL;
    }

    /* Add new entry */
    PackageMap *target_map = NULL;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        /* Remove from all maps first if requested */
        if (remove_first) {
            remove_from_all_app_maps(elm_json, author, name);
        }

        /* Select target map based on is_test and is_direct */
        if (is_test) {
            target_map = is_direct ? elm_json->dependencies_test_direct
                                   : elm_json->dependencies_test_indirect;
        } else {
            target_map = is_direct ? elm_json->dependencies_direct
                                   : elm_json->dependencies_indirect;
        }
    } else {
        /* Package type */
        target_map = is_test ? elm_json->package_test_dependencies
                             : elm_json->package_dependencies;
    }

    if (!target_map) {
        if (constraint) arena_free(constraint);
        return false;
    }

    bool result = package_map_add(target_map, author, name, version_to_add);

    if (constraint) {
        arena_free(constraint);
    }

    return result;
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
    struct CacheConfig *cache,
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
    char *pkg_path = cache_get_package_path((CacheConfig *)cache, author, name, version);
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
    struct CacheConfig *cache,
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
