#include <stdio.h>
#include <string.h>

#include "../../src/package_suggestions.h"
#include "../../src/install_env.h"
#include "../../src/registry.h"
#include "../../src/protocol_v2/solver/v2_registry.h"
#include "../../src/alloc.h"

static int test_v1_suggestion_order(void) {
    RegistryEntry entries[4];
    entries[0].author = "elm";
    entries[0].name = "qrst";
    entries[0].versions = NULL;
    entries[0].version_count = 1;

    entries[1].author = "abc";
    entries[1].name = "qrts";
    entries[1].versions = NULL;
    entries[1].version_count = 1;

    entries[2].author = "abcd";
    entries[2].name = "qrst";
    entries[2].versions = NULL;
    entries[2].version_count = 1;

    entries[3].author = "abc";
    entries[3].name = "qrstu";
    entries[3].versions = NULL;
    entries[3].version_count = 1;

    Registry registry;
    registry.entries = entries;
    registry.entry_count = 4;
    registry.capacity = 4;
    registry.total_versions = 4;

    InstallEnv env = {0};
    env.protocol_mode = PROTOCOL_V1;
    env.registry = &registry;

    PackageSuggestion suggestions[MAX_PACKAGE_SUGGESTIONS];
    size_t count = package_suggest_nearby_from_env(&env, "abc", "qrst", suggestions);

    if (count != 4) {
        fprintf(stderr, "FAIL: expected 4 suggestions, got %zu\n", count);
        return 1;
    }

    if (!(strcmp(suggestions[0].author, "elm") == 0 && strcmp(suggestions[0].name, "qrst") == 0 && suggestions[0].score == 0)) {
        fprintf(stderr, "FAIL: expected elm/qrst as top suggestion with score 0\n");
        return 1;
    }

    if (!(strcmp(suggestions[1].author, "abc") == 0 && strcmp(suggestions[1].name, "qrts") == 0 && suggestions[1].score == 1)) {
        fprintf(stderr, "FAIL: expected transposition candidate in second position\n");
        return 1;
    }

    if (!(strcmp(suggestions[2].author, "abcd") == 0 && strcmp(suggestions[2].name, "qrst") == 0 && suggestions[2].score == 1)) {
        fprintf(stderr, "FAIL: expected insertion candidate in third position\n");
        return 1;
    }

    if (!(strcmp(suggestions[3].author, "abc") == 0 && strcmp(suggestions[3].name, "qrstu") == 0 && suggestions[3].score == 1)) {
        fprintf(stderr, "FAIL: expected final candidate to preserve iteration order\n");
        return 1;
    }

    return 0;
}

static int test_restricted_distance_example(void) {
    RegistryEntry entry;
    entry.author = "alpha";
    entry.name = "ABC";
    entry.versions = NULL;
    entry.version_count = 1;

    Registry registry;
    registry.entries = &entry;
    registry.entry_count = 1;
    registry.capacity = 1;
    registry.total_versions = 1;

    InstallEnv env = {0};
    env.protocol_mode = PROTOCOL_V1;
    env.registry = &registry;

    PackageSuggestion suggestions[MAX_PACKAGE_SUGGESTIONS];
    size_t count = package_suggest_nearby_from_env(&env, "alpha", "CA", suggestions);

    if (count != 1) {
        fprintf(stderr, "FAIL: expected a single suggestion, got %zu\n", count);
        return 1;
    }

    if (suggestions[0].score != 3) {
        fprintf(stderr, "FAIL: expected restricted OSA distance 3 (got %u)\n", suggestions[0].score);
        return 1;
    }

    return 0;
}

static int test_v2_skips_invalid_versions(void) {
    V2PackageVersion invalid_versions[1];
    invalid_versions[0].major = 1;
    invalid_versions[0].minor = 0;
    invalid_versions[0].patch = 0;
    invalid_versions[0].status = V2_STATUS_OBSOLETE;
    invalid_versions[0].license = NULL;
    invalid_versions[0].dependencies = NULL;
    invalid_versions[0].dependency_count = 0;

    V2PackageVersion valid_versions[1];
    valid_versions[0].major = 1;
    valid_versions[0].minor = 0;
    valid_versions[0].patch = 0;
    valid_versions[0].status = V2_STATUS_VALID;
    valid_versions[0].license = NULL;
    valid_versions[0].dependencies = NULL;
    valid_versions[0].dependency_count = 0;

    V2PackageEntry entries[2];
    entries[0].author = "alpha";
    entries[0].name = "skipme";
    entries[0].versions = invalid_versions;
    entries[0].version_count = 1;

    entries[1].author = "foo";
    entries[1].name = "bar";
    entries[1].versions = valid_versions;
    entries[1].version_count = 1;

    V2Registry registry;
    registry.format_version = 0;
    registry.compiler_name = NULL;
    registry.compiler_version = NULL;
    registry.entries = entries;
    registry.entry_count = 2;
    registry.entry_capacity = 2;

    InstallEnv env = {0};
    env.protocol_mode = PROTOCOL_V2;
    env.v2_registry = &registry;

    PackageSuggestion suggestions[MAX_PACKAGE_SUGGESTIONS];
    size_t count = package_suggest_nearby_from_env(&env, "foo", "baz", suggestions);

    if (count != 1) {
        fprintf(stderr, "FAIL: expected only valid V2 entry to be suggested, got %zu\n", count);
        return 1;
    }

    if (!(strcmp(suggestions[0].author, "foo") == 0 && strcmp(suggestions[0].name, "bar") == 0)) {
        fprintf(stderr, "FAIL: unexpected V2 suggestion candidate\n");
        return 1;
    }

    return 0;
}

int main(void) {
    alloc_init();

    int failures = 0;
    failures += test_v1_suggestion_order();
    failures += test_restricted_distance_example();
    failures += test_v2_skips_invalid_versions();

    if (failures == 0) {
        printf("package_suggestions_test: ok\n");
    }

    alloc_shutdown();
    return failures == 0 ? 0 : 1;
}
