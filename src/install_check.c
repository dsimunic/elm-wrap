#include "install_check.h"
#include "elm_json.h"
#include "registry.h"
#include "alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// ANSI color codes for terminal output
#define ANSI_GREEN "\033[1;32m"
#define ANSI_RESET "\033[0m"

typedef struct {
    char *author;
    char *name;
    char *current_version;
    char *latest_minor;
    char *latest_major;
    bool has_minor_upgrade;
    bool has_major_upgrade;
} PackageUpgrade;

/* Parse semantic version into major, minor, patch components */
static bool parse_version(const char *version, int *major, int *minor, int *patch) {
    if (!version) return false;
    
    *major = *minor = *patch = 0;
    
    if (sscanf(version, "%d.%d.%d", major, minor, patch) == 3) {
        return true;
    }
    
    return false;
}

/* Compare two semantic versions
 * Returns: -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2
 */
static int compare_versions(const char *v1, const char *v2) {
    int v1_major, v1_minor, v1_patch;
    int v2_major, v2_minor, v2_patch;
    
    if (!parse_version(v1, &v1_major, &v1_minor, &v1_patch)) return -1;
    if (!parse_version(v2, &v2_major, &v2_minor, &v2_patch)) return -1;
    
    if (v1_major != v2_major) return v1_major < v2_major ? -1 : 1;
    if (v1_minor != v2_minor) return v1_minor < v2_minor ? -1 : 1;
    if (v1_patch != v2_patch) return v1_patch < v2_patch ? -1 : 1;
    
    return 0;
}

/* Check if a version is a minor upgrade (same major version, higher minor/patch) */
static bool is_minor_upgrade(const char *current, const char *candidate) {
    int curr_major, curr_minor, curr_patch;
    int cand_major, cand_minor, cand_patch;
    
    if (!parse_version(current, &curr_major, &curr_minor, &curr_patch)) return false;
    if (!parse_version(candidate, &cand_major, &cand_minor, &cand_patch)) return false;
    
    return (cand_major == curr_major) && (compare_versions(candidate, current) > 0);
}

/* Check if a version is a major upgrade (higher major version) */
static bool is_major_upgrade(const char *current, const char *candidate) {
    int curr_major, curr_minor, curr_patch;
    int cand_major, cand_minor, cand_patch;
    
    if (!parse_version(current, &curr_major, &curr_minor, &curr_patch)) return false;
    if (!parse_version(candidate, &cand_major, &cand_minor, &cand_patch)) return false;
    
    return cand_major > curr_major;
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

/* Compare two package names (case-sensitive) for sorting */
static int compare_package_names(const void *a, const void *b) {
    const PackageUpgrade *upg_a = (const PackageUpgrade *)a;
    const PackageUpgrade *upg_b = (const PackageUpgrade *)b;

    // Compare full package name (author/name)
    char name_a[256], name_b[256];
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
                                        PackageUpgrade **upgrades, int *upgrade_count) {
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

            (*upgrade_count)++;
        }
    }
}

int check_all_upgrades(const char *elm_json_path, Registry *registry) {
    // Validate registry
    if (!registry) {
        fprintf(stderr, "Error: No registry provided\n");
        return 1;
    }

    // Load elm.json
    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        fprintf(stderr, "Error: Could not read %s\n", elm_json_path);
        return 1;
    }
    
    // Collect all upgrades
    PackageUpgrade *upgrades = NULL;
    int upgrade_count = 0;

    if (elm_json->type == ELM_PROJECT_APPLICATION) {
        check_package_map_upgrades(elm_json->dependencies_direct, registry,
                                   &upgrades, &upgrade_count);
        check_package_map_upgrades(elm_json->dependencies_indirect, registry,
                                   &upgrades, &upgrade_count);
        check_package_map_upgrades(elm_json->dependencies_test_direct, registry,
                                   &upgrades, &upgrade_count);
        check_package_map_upgrades(elm_json->dependencies_test_indirect, registry,
                                   &upgrades, &upgrade_count);
    } else if (elm_json->type == ELM_PROJECT_PACKAGE) {
        check_package_map_upgrades(elm_json->package_dependencies, registry,
                                   &upgrades, &upgrade_count);
        check_package_map_upgrades(elm_json->package_test_dependencies, registry,
                                   &upgrades, &upgrade_count);
    }

    // Print results
    if (upgrade_count == 0) {
        printf("No upgrades available. All packages at their latest version\n");
        elm_json_free(elm_json);
        return 100;
    }

    // Sort upgrades by package name, then minor before major
    qsort(upgrades, upgrade_count, sizeof(PackageUpgrade), compare_package_names);

    // Find longest package name for alignment
    size_t max_name_len = 0;
    for (int i = 0; i < upgrade_count; i++) {
        char full_name[256];
        snprintf(full_name, sizeof(full_name), "%s/%s",
                 upgrades[i].author, upgrades[i].name);
        size_t len = strlen(full_name);
        if (len > max_name_len) {
            max_name_len = len;
        }
    }

    // Add 4 spaces padding
    size_t padding = max_name_len + 4;

    printf("Available upgrades:\n\n");

    for (int i = 0; i < upgrade_count; i++) {
        PackageUpgrade *upg = &upgrades[i];
        char full_name[256];
        snprintf(full_name, sizeof(full_name), "%s/%s", upg->author, upg->name);

        // Print minor upgrades first
        if (upg->has_minor_upgrade) {
            printf("[minor] %-*s %s -> %s\n",
                   (int)padding, full_name, upg->current_version, upg->latest_minor);
        }

        // Print major upgrades in bold/green
        if (upg->has_major_upgrade) {
            printf("%s[major] %-*s %s -> %s%s\n",
                   ANSI_GREEN, (int)padding, full_name,
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
