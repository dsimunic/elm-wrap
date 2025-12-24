#include "install_check.h"
#include "elm_json.h"
#include "registry.h"
#include "alloc.h"
#include "constants.h"
#include "fileutil.h"
#include "exit_codes.h"
#include "terminal_colors.h"
#include "shared/log.h"
#include "commands/package/package_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

typedef struct {
    char *author;
    char *name;
    char *current_version;
    char *latest_minor;
    char *latest_major;
    bool has_minor_upgrade;
    bool has_major_upgrade;
    bool is_test_dependency;
} PackageUpgrade;

/* Check for duplicate exposed modules in a package elm.json file.
 * Returns the number of duplicates found (0 if none).
 */
static int check_duplicate_exposed_modules(const char *elm_json_path) {
    char *content = file_read_contents_bounded(elm_json_path, MAX_ELM_JSON_FILE_BYTES, NULL);
    if (!content) {
        return 0;
    }

    /* Find "exposed-modules" key */
    char *exposed = strstr(content, "\"exposed-modules\"");
    if (!exposed) {
        arena_free(content);
        return 0;
    }

    /* Skip past the key to find the value */
    char *value_start = exposed + strlen("\"exposed-modules\"");
    while (*value_start && (*value_start == ':' || isspace((unsigned char)*value_start))) {
        value_start++;
    }

    /* Check if it's an object (categorized) or array (simple) */
    bool is_categorized = (*value_start == '{');

    /* Find the opening bracket of first array */
    char *bracket = strchr(value_start, '[');
    if (!bracket) {
        arena_free(content);
        return 0;
    }

    /* Collect all module names */
    int modules_count = 0;
    int modules_capacity = INITIAL_MODULE_CAPACITY;
    char **modules = arena_malloc(modules_capacity * sizeof(char*));

    /* Parse all arrays (for categorized format, there may be multiple) */
    char *search_pos = bracket;
    char *end_marker = is_categorized ? strchr(value_start, '}') : strchr(bracket, ']');

    while (search_pos && search_pos < end_marker) {
        /* Parse module names within this array */
        char *p = search_pos + 1;
        while (*p) {
            /* Skip whitespace */
            while (*p && isspace((unsigned char)*p)) p++;

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

                    /* Add to list */
                    if (modules_count >= modules_capacity) {
                        modules_capacity *= 2;
                        modules = arena_realloc(modules, modules_capacity * sizeof(char*));
                    }
                    modules[modules_count++] = module;
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
            break;
        }
    }

    /* Check for duplicates */
    int duplicates = 0;
    for (int i = 0; i < modules_count; i++) {
        for (int j = i + 1; j < modules_count; j++) {
            if (strcmp(modules[i], modules[j]) == 0) {
                log_warn("Duplicate exposed module '%s' in elm.json", modules[i]);
                duplicates++;
                break;  /* Only report each duplicate once */
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < modules_count; i++) {
        arena_free(modules[i]);
    }
    arena_free(modules);
    arena_free(content);

    return duplicates;
}

/* Compare two semantic versions
 * Returns: -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2
 */
static int compare_versions(const char *v1_str, const char *v2_str) {
    Version v1, v2;
    if (!version_parse_safe(v1_str, &v1)) return -1;
    if (!version_parse_safe(v2_str, &v2)) return -1;
    return version_compare(&v1, &v2);
}

/* Check if a version is a minor upgrade (same major version, higher minor/patch) */
static bool is_minor_upgrade(const char *current, const char *candidate) {
    Version curr, cand;
    if (!version_parse_safe(current, &curr)) return false;
    if (!version_parse_safe(candidate, &cand)) return false;

    return (cand.major == curr.major) && (version_compare(&cand, &curr) > 0);
}

/* Check if a version is a major upgrade (higher major version) */
static bool is_major_upgrade(const char *current, const char *candidate) {
    Version curr, cand;
    if (!version_parse_safe(current, &curr)) return false;
    if (!version_parse_safe(candidate, &cand)) return false;

    return cand.major > curr.major;
}

/* Find latest versions for a package in registry */
static void find_latest_versions(Registry *registry, const char *author, const char *name,
                                  const char *current_version, char **latest_minor, char **latest_major) {
    *latest_minor = NULL;
    *latest_major = NULL;

    RegistryEntry *entry = registry_find(registry, author, name);
    if (!entry) {
        return;
    }

    for (size_t i = 0; i < entry->version_count; i++) {
        char *ver_str = version_to_string(&entry->versions[i]);
        if (!ver_str) continue;

        // Check for minor upgrade
        if (is_minor_upgrade(current_version, ver_str)) {
            if (!*latest_minor || compare_versions(ver_str, *latest_minor) > 0) {
                arena_free(*latest_minor);
                *latest_minor = arena_strdup(ver_str);
            }
        }

        // Check for major upgrade
        if (is_major_upgrade(current_version, ver_str)) {
            if (!*latest_major || compare_versions(ver_str, *latest_major) > 0) {
                arena_free(*latest_major);
                *latest_major = arena_strdup(ver_str);
            }
        }

        arena_free(ver_str);
    }
}

/* Find versions beyond constraint upper bound for a package dependency.
 * For packages, the "version" field is a constraint like "1.0.0 <= v < 2.0.0".
 * We only care about versions that exceed the upper bound (e.g., 2.x.x or higher).
 */
static void find_versions_beyond_constraint(Registry *registry, const char *author, const char *name,
                                            const char *constraint, char **latest_beyond) {
    *latest_beyond = NULL;

    VersionRange range;
    if (!version_parse_constraint(constraint, &range)) {
        return; /* Could not parse constraint */
    }

    RegistryEntry *entry = registry_find(registry, author, name);
    if (!entry) {
        return;
    }

    for (size_t i = 0; i < entry->version_count; i++) {
        /* Check if version is at or beyond the upper bound */
        if (version_compare(&entry->versions[i], &range.upper.v) >= 0) {
            char *ver_str = version_to_string(&entry->versions[i]);
            if (!ver_str) continue;

            if (!*latest_beyond || compare_versions(ver_str, *latest_beyond) > 0) {
                arena_free(*latest_beyond);
                *latest_beyond = arena_strdup(ver_str);
            }

            arena_free(ver_str);
        }
    }
}

/* Compare two package names (case-sensitive) for sorting */
static int compare_package_names(const void *a, const void *b) {
    const PackageUpgrade *upg_a = (const PackageUpgrade *)a;
    const PackageUpgrade *upg_b = (const PackageUpgrade *)b;

    // First, non-test dependencies come before test dependencies
    if (upg_a->is_test_dependency != upg_b->is_test_dependency) {
        return upg_a->is_test_dependency ? 1 : -1;
    }

    // Compare full package name (author/name)
    char name_a[MAX_PACKAGE_NAME_LENGTH], name_b[MAX_PACKAGE_NAME_LENGTH];
    snprintf(name_a, sizeof(name_a), "%s/%s", upg_a->author, upg_a->name);
    snprintf(name_b, sizeof(name_b), "%s/%s", upg_b->author, upg_b->name);

    int cmp = strcmp(name_a, name_b);
    if (cmp != 0) return cmp;

    // Same package: minor upgrades come before major upgrades
    // has_minor_upgrade && !has_major_upgrade should come first
    // Both or neither should maintain order
    if (upg_a->has_minor_upgrade && !upg_a->has_major_upgrade &&
        upg_b->has_major_upgrade && !upg_b->has_minor_upgrade) {
        return -1;
    }
    if (upg_a->has_major_upgrade && !upg_a->has_minor_upgrade &&
        upg_b->has_minor_upgrade && !upg_b->has_major_upgrade) {
        return 1;
    }

    return 0;
}

/* Check a package map for upgrades */
static void check_package_map_upgrades(PackageMap *map, Registry *registry,
                                        PackageUpgrade **upgrades, int *upgrade_count, bool is_test) {
    if (!map) return;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];

        char *latest_minor = NULL;
        char *latest_major = NULL;

        find_latest_versions(registry, pkg->author, pkg->name, pkg->version,
                            &latest_minor, &latest_major);

        if (latest_minor || latest_major) {
            *upgrades = arena_realloc(*upgrades, (*upgrade_count + 1) * sizeof(PackageUpgrade));
            PackageUpgrade *upg = &(*upgrades)[*upgrade_count];

            upg->author = arena_strdup(pkg->author);
            upg->name = arena_strdup(pkg->name);
            upg->current_version = arena_strdup(pkg->version);
            upg->latest_minor = latest_minor;
            upg->latest_major = latest_major;
            upg->has_minor_upgrade = (latest_minor != NULL);
            upg->has_major_upgrade = (latest_major != NULL);
            upg->is_test_dependency = is_test;

            (*upgrade_count)++;
        }
    }
}

/* Check a package map for versions beyond constraint (for package elm.json).
 * Only reports upgrades that are beyond the declared upper bound.
 */
static void check_package_constraint_upgrades(PackageMap *map, Registry *registry,
                                              PackageUpgrade **upgrades, int *upgrade_count, bool is_test) {
    if (!map) return;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];

        char *latest_beyond = NULL;

        find_versions_beyond_constraint(registry, pkg->author, pkg->name, pkg->version,
                                        &latest_beyond);

        if (latest_beyond) {
            *upgrades = arena_realloc(*upgrades, (*upgrade_count + 1) * sizeof(PackageUpgrade));
            PackageUpgrade *upg = &(*upgrades)[*upgrade_count];

            upg->author = arena_strdup(pkg->author);
            upg->name = arena_strdup(pkg->name);
            upg->current_version = arena_strdup(pkg->version);
            upg->latest_minor = NULL;
            upg->latest_major = latest_beyond;
            upg->has_minor_upgrade = false;
            upg->has_major_upgrade = true;
            upg->is_test_dependency = is_test;

            (*upgrade_count)++;
        }
    }
}

int check_all_upgrades(const char *elm_json_path, Registry *registry, size_t max_name_len) {
    // Validate registry
    if (!registry) {
        log_error("No registry provided");
        return 1;
    }

    // Load elm.json
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_error("Could not read %s", elm_json_path);
        return 1;
    }

    // Check for duplicate exposed modules in package elm.json
    if (elm_json->type == ELM_PROJECT_PACKAGE) {
        check_duplicate_exposed_modules(elm_json_path);
    }

    // Collect all upgrades
    PackageUpgrade *upgrades = NULL;
    int upgrade_count = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        check_package_map_upgrades(elm_json->dependencies_direct, registry,
                                   &upgrades, &upgrade_count, false);
        check_package_map_upgrades(elm_json->dependencies_indirect, registry,
                                   &upgrades, &upgrade_count, false);
        check_package_map_upgrades(elm_json->dependencies_test_direct, registry,
                                   &upgrades, &upgrade_count, true);
        check_package_map_upgrades(elm_json->dependencies_test_indirect, registry,
                                   &upgrades, &upgrade_count, true);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        /* For packages, only report versions beyond the declared constraint upper bound */
        check_package_constraint_upgrades(elm_json->package_dependencies, registry,
                                          &upgrades, &upgrade_count, false);
        check_package_constraint_upgrades(elm_json->package_test_dependencies, registry,
                                          &upgrades, &upgrade_count, true);
    }

    // Print results
    if (upgrade_count == 0) {
        if (elm_json->type == ELM_PROJECT_PACKAGE) {
            printf("No upgrades beyond declared version constraints\n");
        } else {
            printf("No upgrades available. All packages at their latest version\n");
        }
        elm_json_free(elm_json);
        return EXIT_NO_UPGRADES_AVAILABLE;
    }

    // Sort upgrades by package name, then minor before major
    qsort(upgrades, upgrade_count, sizeof(PackageUpgrade), compare_package_names);

    // Calculate max_name_len from upgrades if not provided, or update if any upgrade name is longer
    for (int i = 0; i < upgrade_count; i++) {
        char full_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(full_name, sizeof(full_name), "%s/%s",
                 upgrades[i].author, upgrades[i].name);
        size_t len = strlen(full_name);
        if (len > max_name_len) {
            max_name_len = len;
        }
    }

    printf("Available upgrades:\n\n");

    bool last_was_test = false;
    for (int i = 0; i < upgrade_count; i++) {
        PackageUpgrade *upg = &upgrades[i];

        // Add separator between non-test and test packages
        if (i > 0 && upg->is_test_dependency && !last_was_test) {
            printf("\n");
        }
        last_was_test = upg->is_test_dependency;

        char full_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(full_name, sizeof(full_name), "%s/%s", upg->author, upg->name);

        // Print minor upgrades first
        if (upg->has_minor_upgrade) {
            printf("  %-*s  %s -> %s\n",
                   (int)max_name_len, full_name, upg->current_version, upg->latest_minor);
        }

        // Print major upgrades in bold/green
        if (upg->has_major_upgrade) {
            printf("%s  %-*s  %s -> %s (major)%s\n",
                   ANSI_BRIGHT_GREEN, (int)max_name_len, full_name,
                   upg->current_version, upg->latest_major, ANSI_RESET);
        }

        // Cleanup
        arena_free(upg->author);
        arena_free(upg->name);
        arena_free(upg->current_version);
        arena_free(upg->latest_minor);
        arena_free(upg->latest_major);
    }

    arena_free(upgrades);
    elm_json_free(elm_json);

    return 0;
}

/* ========== V2 Protocol Support ========== */

#include "protocol_v2/solver/v2_registry.h"

/* Find latest versions for a package in V2 registry */
static void find_latest_versions_v2(V2Registry *registry, const char *author, const char *name,
                                     const char *current_version, char **latest_minor, char **latest_major) {
    *latest_minor = NULL;
    *latest_major = NULL;

    V2PackageEntry *entry = v2_registry_find(registry, author, name);
    if (!entry) {
        return;
    }

    for (size_t i = 0; i < entry->version_count; i++) {
        V2PackageVersion *v = &entry->versions[i];
        
        /* Skip non-valid versions */
        if (v->status != V2_STATUS_VALID) {
            continue;
        }
        
        /* Format version string */
        char *ver_str = version_format(v->major, v->minor, v->patch);
        if (!ver_str) continue;

        /* Check for minor upgrade */
        if (is_minor_upgrade(current_version, ver_str)) {
            if (!*latest_minor || compare_versions(ver_str, *latest_minor) > 0) {
                arena_free(*latest_minor);
                *latest_minor = arena_strdup(ver_str);
            }
        }

        /* Check for major upgrade */
        if (is_major_upgrade(current_version, ver_str)) {
            if (!*latest_major || compare_versions(ver_str, *latest_major) > 0) {
                arena_free(*latest_major);
                *latest_major = arena_strdup(ver_str);
            }
        }

        arena_free(ver_str);
    }
}

/* Check a package map for upgrades using V2 registry */
static void check_package_map_upgrades_v2(PackageMap *map, V2Registry *registry,
                                          PackageUpgrade **upgrades, int *upgrade_count, bool is_test) {
    if (!map) return;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];

        char *latest_minor = NULL;
        char *latest_major = NULL;

        find_latest_versions_v2(registry, pkg->author, pkg->name, pkg->version,
                                &latest_minor, &latest_major);

        if (latest_minor || latest_major) {
            *upgrades = arena_realloc(*upgrades, (*upgrade_count + 1) * sizeof(PackageUpgrade));
            PackageUpgrade *upg = &(*upgrades)[*upgrade_count];

            upg->author = arena_strdup(pkg->author);
            upg->name = arena_strdup(pkg->name);
            upg->current_version = arena_strdup(pkg->version);
            upg->latest_minor = latest_minor;
            upg->latest_major = latest_major;
            upg->has_minor_upgrade = (latest_minor != NULL);
            upg->has_major_upgrade = (latest_major != NULL);
            upg->is_test_dependency = is_test;

            (*upgrade_count)++;
        }
    }
}

/* Find versions beyond constraint upper bound for a package dependency (V2 registry).
 * For packages, the "version" field is a constraint like "1.0.0 <= v < 2.0.0".
 * We only care about versions that exceed the upper bound (e.g., 2.x.x or higher).
 */
static void find_versions_beyond_constraint_v2(V2Registry *registry, const char *author, const char *name,
                                               const char *constraint, char **latest_beyond) {
    *latest_beyond = NULL;

    VersionRange range;
    if (!version_parse_constraint(constraint, &range)) {
        return; /* Could not parse constraint */
    }

    V2PackageEntry *entry = v2_registry_find(registry, author, name);
    if (!entry) {
        return;
    }

    for (size_t i = 0; i < entry->version_count; i++) {
        V2PackageVersion *v = &entry->versions[i];

        /* Skip non-valid versions */
        if (v->status != V2_STATUS_VALID) {
            continue;
        }

        /* Check if version is at or beyond the upper bound */
        Version v_struct = {(uint16_t)v->major, (uint16_t)v->minor, (uint16_t)v->patch};
        if (version_compare(&v_struct, &range.upper.v) >= 0) {
            char *ver_str = version_format(v->major, v->minor, v->patch);
            if (ver_str) {
                if (!*latest_beyond || compare_versions(ver_str, *latest_beyond) > 0) {
                    arena_free(*latest_beyond);
                    *latest_beyond = arena_strdup(ver_str);
                }
                arena_free(ver_str);
            }
        }
    }
}

/* Check a package map for versions beyond constraint (for package elm.json, V2 registry).
 * Only reports upgrades that are beyond the declared upper bound.
 */
static void check_package_constraint_upgrades_v2(PackageMap *map, V2Registry *registry,
                                                 PackageUpgrade **upgrades, int *upgrade_count, bool is_test) {
    if (!map) return;

    for (int i = 0; i < map->count; i++) {
        Package *pkg = &map->packages[i];

        char *latest_beyond = NULL;

        find_versions_beyond_constraint_v2(registry, pkg->author, pkg->name, pkg->version,
                                           &latest_beyond);

        if (latest_beyond) {
            *upgrades = arena_realloc(*upgrades, (*upgrade_count + 1) * sizeof(PackageUpgrade));
            PackageUpgrade *upg = &(*upgrades)[*upgrade_count];

            upg->author = arena_strdup(pkg->author);
            upg->name = arena_strdup(pkg->name);
            upg->current_version = arena_strdup(pkg->version);
            upg->latest_minor = NULL;
            upg->latest_major = latest_beyond;
            upg->has_minor_upgrade = false;
            upg->has_major_upgrade = true;
            upg->is_test_dependency = is_test;

            (*upgrade_count)++;
        }
    }
}

int check_all_upgrades_v2(const char *elm_json_path, V2Registry *registry, size_t max_name_len) {
    /* Validate registry */
    if (!registry) {
        log_error("No V2 registry provided");
        return 1;
    }

    /* Load elm.json */
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        log_error("Could not read %s", elm_json_path);
        return 1;
    }

    /* Check for duplicate exposed modules in package elm.json */
    if (elm_json->type == ELM_PROJECT_PACKAGE) {
        check_duplicate_exposed_modules(elm_json_path);
    }

    /* Collect all upgrades */
    PackageUpgrade *upgrades = NULL;
    int upgrade_count = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        check_package_map_upgrades_v2(elm_json->dependencies_direct, registry,
                                      &upgrades, &upgrade_count, false);
        check_package_map_upgrades_v2(elm_json->dependencies_indirect, registry,
                                      &upgrades, &upgrade_count, false);
        check_package_map_upgrades_v2(elm_json->dependencies_test_direct, registry,
                                      &upgrades, &upgrade_count, true);
        check_package_map_upgrades_v2(elm_json->dependencies_test_indirect, registry,
                                      &upgrades, &upgrade_count, true);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        /* For packages, only report versions beyond the declared constraint upper bound */
        check_package_constraint_upgrades_v2(elm_json->package_dependencies, registry,
                                             &upgrades, &upgrade_count, false);
        check_package_constraint_upgrades_v2(elm_json->package_test_dependencies, registry,
                                             &upgrades, &upgrade_count, true);
    }

    /* Print results */
    if (upgrade_count == 0) {
        if (elm_json->type == ELM_PROJECT_PACKAGE) {
            printf("No upgrades beyond declared version constraints\n");
        } else {
            printf("No upgrades available. All packages at their latest version\n");
        }
        elm_json_free(elm_json);
        return EXIT_NO_UPGRADES_AVAILABLE;
    }

    /* Sort upgrades by package name, then minor before major */
    qsort(upgrades, upgrade_count, sizeof(PackageUpgrade), compare_package_names);

    /* Calculate max_name_len from upgrades if not provided, or update if any upgrade name is longer */
    for (int i = 0; i < upgrade_count; i++) {
        char full_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(full_name, sizeof(full_name), "%s/%s",
                 upgrades[i].author, upgrades[i].name);
        size_t len = strlen(full_name);
        if (len > max_name_len) {
            max_name_len = len;
        }
    }

    printf("Available upgrades:\n\n");

    bool last_was_test = false;
    for (int i = 0; i < upgrade_count; i++) {
        PackageUpgrade *upg = &upgrades[i];

        // Add separator between non-test and test packages
        if (i > 0 && upg->is_test_dependency && !last_was_test) {
            printf("\n");
        }
        last_was_test = upg->is_test_dependency;

        char full_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(full_name, sizeof(full_name), "%s/%s", upg->author, upg->name);

        /* Print minor upgrades first */
        if (upg->has_minor_upgrade) {
            printf("  %-*s  %s -> %s\n",
                   (int)max_name_len, full_name, upg->current_version, upg->latest_minor);
        }

        /* Print major upgrades in bold/green */
        if (upg->has_major_upgrade) {
            printf("%s  %-*s  %s -> %s (major)%s\n",
                   ANSI_BRIGHT_GREEN, (int)max_name_len, full_name,
                   upg->current_version, upg->latest_major, ANSI_RESET);
        }

        /* Cleanup */
        arena_free(upg->author);
        arena_free(upg->name);
        arena_free(upg->current_version);
        arena_free(upg->latest_minor);
        arena_free(upg->latest_major);
    }

    arena_free(upgrades);
    elm_json_free(elm_json);

    return 0;
}

/* ========== Max Name Length Helpers ========== */

size_t get_max_upgrade_name_len(const char *elm_json_path, Registry *registry) {
    if (!registry) return 0;

    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) return 0;

    PackageUpgrade *upgrades = NULL;
    int upgrade_count = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        check_package_map_upgrades(elm_json->dependencies_direct, registry,
                                   &upgrades, &upgrade_count, false);
        check_package_map_upgrades(elm_json->dependencies_indirect, registry,
                                   &upgrades, &upgrade_count, false);
        check_package_map_upgrades(elm_json->dependencies_test_direct, registry,
                                   &upgrades, &upgrade_count, true);
        check_package_map_upgrades(elm_json->dependencies_test_indirect, registry,
                                   &upgrades, &upgrade_count, true);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        check_package_constraint_upgrades(elm_json->package_dependencies, registry,
                                          &upgrades, &upgrade_count, false);
        check_package_constraint_upgrades(elm_json->package_test_dependencies, registry,
                                          &upgrades, &upgrade_count, true);
    }

    size_t max_len = 0;
    for (int i = 0; i < upgrade_count; i++) {
        char full_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(full_name, sizeof(full_name), "%s/%s",
                 upgrades[i].author, upgrades[i].name);
        size_t len = strlen(full_name);
        if (len > max_len) {
            max_len = len;
        }
        arena_free(upgrades[i].author);
        arena_free(upgrades[i].name);
        arena_free(upgrades[i].current_version);
        arena_free(upgrades[i].latest_minor);
        arena_free(upgrades[i].latest_major);
    }

    arena_free(upgrades);
    elm_json_free(elm_json);

    return max_len;
}

size_t get_max_upgrade_name_len_v2(const char *elm_json_path, V2Registry *registry) {
    if (!registry) return 0;

    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) return 0;

    PackageUpgrade *upgrades = NULL;
    int upgrade_count = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        check_package_map_upgrades_v2(elm_json->dependencies_direct, registry,
                                      &upgrades, &upgrade_count, false);
        check_package_map_upgrades_v2(elm_json->dependencies_indirect, registry,
                                      &upgrades, &upgrade_count, false);
        check_package_map_upgrades_v2(elm_json->dependencies_test_direct, registry,
                                      &upgrades, &upgrade_count, true);
        check_package_map_upgrades_v2(elm_json->dependencies_test_indirect, registry,
                                      &upgrades, &upgrade_count, true);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        check_package_constraint_upgrades_v2(elm_json->package_dependencies, registry,
                                             &upgrades, &upgrade_count, false);
        check_package_constraint_upgrades_v2(elm_json->package_test_dependencies, registry,
                                             &upgrades, &upgrade_count, true);
    }

    size_t max_len = 0;
    for (int i = 0; i < upgrade_count; i++) {
        char full_name[MAX_PACKAGE_NAME_LENGTH];
        snprintf(full_name, sizeof(full_name), "%s/%s",
                 upgrades[i].author, upgrades[i].name);
        size_t len = strlen(full_name);
        if (len > max_len) {
            max_len = len;
        }
        arena_free(upgrades[i].author);
        arena_free(upgrades[i].name);
        arena_free(upgrades[i].current_version);
        arena_free(upgrades[i].latest_minor);
        arena_free(upgrades[i].latest_major);
    }

    arena_free(upgrades);
    elm_json_free(elm_json);

    return max_len;
}