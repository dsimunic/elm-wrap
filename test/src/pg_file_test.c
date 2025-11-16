#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "../../src/pgsolver/pg_core.h"
#include "../../src/alloc.h"
#include "vendor/jsmn/jsmn.h"

/*
 * This file provides a test runner that reads PubGrub test cases from JSON
 * files and executes them against the solver implementation.
 */

#define MAX_PACKAGES 32
#define MAX_VERSIONS 16
#define MAX_DEPS 8
#define MAX_TOKENS 2048
#define MAX_PACKAGE_NAME 64

typedef struct {
    char name[MAX_PACKAGE_NAME];
    PgPackageId id;
} PackageMapping;

typedef struct {
    PgPackageId pkg;
    PgVersionRange range;
} TestDependency;

typedef struct {
    PgVersion version;
    int dep_count;
    TestDependency deps[MAX_DEPS];
} TestVersionEntry;

typedef struct {
    PgPackageId pkg;
    char name[MAX_PACKAGE_NAME];
    int version_count;
    TestVersionEntry versions[MAX_VERSIONS];
} TestPackageEntry;

typedef struct {
    int package_count;
    TestPackageEntry packages[MAX_PACKAGES];
    PackageMapping mappings[MAX_PACKAGES];
    int mapping_count;
} TestProviderCtx;

typedef struct {
    char name[128];
    char description[256];
    TestProviderCtx ctx;
    int root_dep_count;
    TestDependency root_deps[MAX_DEPS];
    bool expect_success;
    int expected_solution_count;
    struct {
        PgPackageId pkg;
        PgVersion version;
    } expected_solution[MAX_PACKAGES];
} TestCase;

static PgVersion make_version(int major, int minor, int patch) {
    PgVersion v;
    v.major = major;
    v.minor = minor;
    v.patch = patch;
    return v;
}

static bool parse_version(const char *str, PgVersion *out) {
    int major, minor, patch;
    if (sscanf(str, "%d.%d.%d", &major, &minor, &patch) != 3) {
        return false;
    }
    *out = make_version(major, minor, patch);
    return true;
}

static PgVersionRange parse_range(const char *str) {
    /* Simple parser for common patterns: ^X.Y.Z, >=X.Y.Z, any */
    if (strcmp(str, "any") == 0) {
        return pg_range_any();
    }

    if (str[0] == '^') {
        PgVersion v;
        if (parse_version(str + 1, &v)) {
            return pg_range_until_next_major(v);
        }
    }

    if (strncmp(str, ">=", 2) == 0) {
        PgVersion v;
        if (parse_version(str + 2, &v)) {
            /* For >=X.Y.Z, use the range from X.Y.Z to next major version */
            /* Since we don't have pg_range_at_least, approximate with until_next_major */
            return pg_range_until_next_major(v);
        }
    }

    /* Exact match */
    PgVersion v;
    if (parse_version(str, &v)) {
        return pg_range_exact(v);
    }

    return pg_range_any();
}

static void ctx_init(TestProviderCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static PgPackageId ctx_get_or_create_package_id(TestProviderCtx *ctx, const char *name) {
    /* Check if we already have a mapping */
    for (int i = 0; i < ctx->mapping_count; i++) {
        if (strcmp(ctx->mappings[i].name, name) == 0) {
            return ctx->mappings[i].id;
        }
    }

    /* Create new mapping */
    if (ctx->mapping_count >= MAX_PACKAGES) {
        return -1;
    }

    PgPackageId id = ctx->mapping_count;
    strncpy(ctx->mappings[ctx->mapping_count].name, name, MAX_PACKAGE_NAME - 1);
    ctx->mappings[ctx->mapping_count].name[MAX_PACKAGE_NAME - 1] = '\0';
    ctx->mappings[ctx->mapping_count].id = id;
    ctx->mapping_count++;

    return id;
}

static TestPackageEntry *ctx_add_package(TestProviderCtx *ctx, const char *name, PgPackageId id) {
    if (ctx->package_count >= MAX_PACKAGES) {
        return NULL;
    }
    TestPackageEntry *entry = &ctx->packages[ctx->package_count++];
    entry->pkg = id;
    strncpy(entry->name, name, MAX_PACKAGE_NAME - 1);
    entry->name[MAX_PACKAGE_NAME - 1] = '\0';
    entry->version_count = 0;
    return entry;
}

static TestPackageEntry *ctx_find_package(TestProviderCtx *ctx, PgPackageId pkg) {
    for (int i = 0; i < ctx->package_count; i++) {
        if (ctx->packages[i].pkg == pkg) {
            return &ctx->packages[i];
        }
    }
    return NULL;
}

static TestVersionEntry *pkg_add_version(TestPackageEntry *pkg, PgVersion version) {
    if (!pkg || pkg->version_count >= MAX_VERSIONS) {
        return NULL;
    }
    TestVersionEntry *entry = &pkg->versions[pkg->version_count++];
    entry->version = version;
    entry->dep_count = 0;
    return entry;
}

static TestVersionEntry *pkg_find_version(TestPackageEntry *pkg, PgVersion version) {
    if (!pkg) {
        return NULL;
    }
    for (int i = 0; i < pkg->version_count; i++) {
        if (pg_version_compare(pkg->versions[i].version, version) == 0) {
            return &pkg->versions[i];
        }
    }
    return NULL;
}

static void version_add_dependency(
    TestVersionEntry *version,
    PgPackageId dep_pkg,
    PgVersionRange range
) {
    if (!version || version->dep_count >= MAX_DEPS) {
        return;
    }
    version->deps[version->dep_count].pkg = dep_pkg;
    version->deps[version->dep_count].range = range;
    version->dep_count++;
}

static int test_provider_get_versions(
    void *ctx_ptr,
    PgPackageId pkg,
    PgVersion *out_versions,
    size_t out_capacity
) {
    TestProviderCtx *ctx = (TestProviderCtx *)ctx_ptr;
    if (!ctx || !out_versions || out_capacity == 0) {
        return 0;
    }

    TestPackageEntry *entry = ctx_find_package(ctx, pkg);
    if (!entry) {
        return 0;
    }

    int count = entry->version_count;
    if (count > (int)out_capacity) {
        count = (int)out_capacity;
    }
    for (int i = 0; i < count; i++) {
        out_versions[i] = entry->versions[i].version;
    }
    return count;
}

static int test_provider_get_dependencies(
    void *ctx_ptr,
    PgPackageId pkg,
    PgVersion version,
    PgPackageId *out_pkgs,
    PgVersionRange *out_ranges,
    size_t out_capacity
) {
    TestProviderCtx *ctx = (TestProviderCtx *)ctx_ptr;
    if (!ctx || !out_pkgs || !out_ranges || out_capacity == 0) {
        return 0;
    }

    TestPackageEntry *entry = ctx_find_package(ctx, pkg);
    TestVersionEntry *ver = pkg_find_version(entry, version);
    if (!ver) {
        return 0;
    }

    int count = ver->dep_count;
    if (count > (int)out_capacity) {
        count = (int)out_capacity;
    }
    for (int i = 0; i < count; i++) {
        out_pkgs[i] = ver->deps[i].pkg;
        out_ranges[i] = ver->deps[i].range;
    }
    return count;
}

static PgDependencyProvider make_test_provider(void) {
    PgDependencyProvider provider;
    provider.get_versions = test_provider_get_versions;
    provider.get_dependencies = test_provider_get_dependencies;
    return provider;
}

/* Name resolver for error reporting */
static const char *test_name_resolver(void *ctx, PgPackageId pkg) {
    TestProviderCtx *test_ctx = (TestProviderCtx *)ctx;
    if (!test_ctx || pkg < 0 || pkg >= test_ctx->mapping_count) {
        return "<unknown>";
    }
    return test_ctx->mappings[pkg].name;
}

/* JSON parsing helpers */
static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

static void extract_string(const char *json, jsmntok_t *tok, char *out, size_t out_size) {
    size_t len = tok->end - tok->start;
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, json + tok->start, len);
    out[len] = '\0';
}

static bool parse_test_file(const char *filepath, TestCase *test) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json = arena_malloc(fsize + 1);
    if (!json) {
        fclose(f);
        return false;
    }

    fread(json, 1, fsize, f);
    fclose(f);
    json[fsize] = '\0';

    jsmn_parser parser;
    jsmntok_t tokens[MAX_TOKENS];

    jsmn_init(&parser);
    int r = jsmn_parse(&parser, json, fsize, tokens, MAX_TOKENS);

    if (r < 0) {
        fprintf(stderr, "Failed to parse JSON: %d\n", r);
        arena_free(json);
        return false;
    }

    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        fprintf(stderr, "Root element must be an object\n");
        arena_free(json);
        return false;
    }

    memset(test, 0, sizeof(*test));
    ctx_init(&test->ctx);

    bool has_name = false;

    /* Parse top-level fields */
    for (int i = 1; i < r; i++) {
        if (jsoneq(json, &tokens[i], "name") == 0) {
            extract_string(json, &tokens[i + 1], test->name, sizeof(test->name));
            has_name = true;
            i++;
        } else if (jsoneq(json, &tokens[i], "description") == 0) {
            extract_string(json, &tokens[i + 1], test->description, sizeof(test->description));
            i++;
        } else if (jsoneq(json, &tokens[i], "expected") == 0) {
            char expected[32];
            extract_string(json, &tokens[i + 1], expected, sizeof(expected));
            test->expect_success = (strcmp(expected, "success") == 0);
            i++;
        } else if (jsoneq(json, &tokens[i], "packages") == 0) {
            /* Parse packages object */
            jsmntok_t *packages_tok = &tokens[i + 1];
            if (packages_tok->type != JSMN_OBJECT) {
                arena_free(json);
                return false;
            }

            int pkg_idx = i + 2;
            for (int p = 0; p < packages_tok->size; p++) {
                char pkg_name[MAX_PACKAGE_NAME];
                extract_string(json, &tokens[pkg_idx], pkg_name, sizeof(pkg_name));

                PgPackageId pkg_id = ctx_get_or_create_package_id(&test->ctx, pkg_name);
                TestPackageEntry *pkg_entry = ctx_add_package(&test->ctx, pkg_name, pkg_id);

                jsmntok_t *pkg_obj = &tokens[pkg_idx + 1];
                int field_idx = pkg_idx + 2;

                for (int f = 0; f < pkg_obj->size; f++) {
                    if (jsoneq(json, &tokens[field_idx], "versions") == 0) {
                        jsmntok_t *versions_arr = &tokens[field_idx + 1];
                        int ver_idx = field_idx + 2;

                        for (int v = 0; v < versions_arr->size; v++) {
                            char ver_str[32];
                            extract_string(json, &tokens[ver_idx], ver_str, sizeof(ver_str));
                            PgVersion ver;
                            if (parse_version(ver_str, &ver)) {
                                pkg_add_version(pkg_entry, ver);
                            }
                            ver_idx++;
                        }
                        field_idx = ver_idx;
                    } else if (jsoneq(json, &tokens[field_idx], "dependencies") == 0) {
                        jsmntok_t *deps_obj = &tokens[field_idx + 1];
                        int dep_idx = field_idx + 2;

                        for (int d = 0; d < deps_obj->size; d++) {
                            char ver_str[32];
                            extract_string(json, &tokens[dep_idx], ver_str, sizeof(ver_str));
                            PgVersion ver;

                            if (parse_version(ver_str, &ver)) {
                                TestVersionEntry *ver_entry = pkg_find_version(pkg_entry, ver);
                                jsmntok_t *ver_deps_obj = &tokens[dep_idx + 1];
                                int ver_dep_idx = dep_idx + 2;

                                for (int vd = 0; vd < ver_deps_obj->size; vd++) {
                                    char dep_pkg_name[MAX_PACKAGE_NAME];
                                    char dep_range_str[64];

                                    extract_string(json, &tokens[ver_dep_idx], dep_pkg_name, sizeof(dep_pkg_name));
                                    extract_string(json, &tokens[ver_dep_idx + 1], dep_range_str, sizeof(dep_range_str));

                                    PgPackageId dep_pkg_id = ctx_get_or_create_package_id(&test->ctx, dep_pkg_name);
                                    PgVersionRange range = parse_range(dep_range_str);

                                    version_add_dependency(ver_entry, dep_pkg_id, range);

                                    ver_dep_idx += 2;
                                }
                                dep_idx = ver_dep_idx;
                            } else {
                                /* Skip this version's dependencies */
                                jsmntok_t *ver_deps_obj = &tokens[dep_idx + 1];
                                dep_idx += 2 + (ver_deps_obj->size * 2);
                            }
                        }
                        field_idx = dep_idx;
                    } else {
                        /* Skip unknown field */
                        field_idx += 2;
                    }
                }
                pkg_idx = field_idx;
            }
            i = pkg_idx - 1;
        } else if (jsoneq(json, &tokens[i], "root_dependencies") == 0) {
            jsmntok_t *root_deps_obj = &tokens[i + 1];
            int root_dep_idx = i + 2;

            for (int rd = 0; rd < root_deps_obj->size; rd++) {
                char dep_pkg_name[MAX_PACKAGE_NAME];
                char dep_range_str[64];

                extract_string(json, &tokens[root_dep_idx], dep_pkg_name, sizeof(dep_pkg_name));
                extract_string(json, &tokens[root_dep_idx + 1], dep_range_str, sizeof(dep_range_str));

                PgPackageId dep_pkg_id = ctx_get_or_create_package_id(&test->ctx, dep_pkg_name);
                PgVersionRange range = parse_range(dep_range_str);

                test->root_deps[test->root_dep_count].pkg = dep_pkg_id;
                test->root_deps[test->root_dep_count].range = range;
                test->root_dep_count++;

                root_dep_idx += 2;
            }
            i = root_dep_idx - 1;
        } else {
            /* Skip unknown top-level field */
            i++;
        }
    }

    arena_free(json);

    if (!has_name) {
        /* Not a test file, skip it */
        return false;
    }

    return true;
}

static bool run_test(const char *filepath) {
    TestCase test;

    if (!parse_test_file(filepath, &test)) {
        /* Skip files that don't match our test format */
        return true;
    }

    printf("[pg_file_test] Running: %s\n", test.name);

    PgDependencyProvider provider = make_test_provider();
    PgPackageId root_pkg = ctx_get_or_create_package_id(&test.ctx, "root");

    PgSolver *solver = pg_solver_new(
        provider,
        &test.ctx,
        root_pkg,
        make_version(1, 0, 0)
    );

    if (!solver) {
        fprintf(stderr, "[pg_file_test] Failed to create solver\n");
        return false;
    }

    /* Add root dependencies */
    for (int i = 0; i < test.root_dep_count; i++) {
        if (!pg_solver_add_root_dependency(solver, test.root_deps[i].pkg, test.root_deps[i].range)) {
            fprintf(stderr, "[pg_file_test] Failed to add root dependency\n");
            pg_solver_free(solver);
            return false;
        }
    }

    PgSolverStatus status = pg_solver_solve(solver);

    bool test_passed = false;
    if (test.expect_success) {
        if (status == PG_SOLVER_OK) {
            test_passed = true;
        } else {
            fprintf(stderr, "[pg_file_test] Expected success but got status %d\n", status);
            if (status == PG_SOLVER_NO_SOLUTION) {
                char error_msg[4096];
                if (pg_solver_explain_failure(solver, test_name_resolver, &test.ctx,
                                               error_msg, sizeof(error_msg))) {
                    fprintf(stderr, "\n%s\n", error_msg);
                }
            }
        }
    } else {
        if (status == PG_SOLVER_NO_SOLUTION) {
            test_passed = true;
            /* Print error explanation for failed tests */
            char error_msg[4096];
            if (pg_solver_explain_failure(solver, test_name_resolver, &test.ctx,
                                           error_msg, sizeof(error_msg))) {
                printf("\nError explanation for test '%s':\n%s\n", test.name, error_msg);
            }
        } else {
            fprintf(stderr, "[pg_file_test] Expected conflict but got status %d\n", status);
        }
    }

    pg_solver_free(solver);

    if (test_passed) {
        printf("[pg_file_test] ✓ PASSED: %s\n", test.name);
    } else {
        printf("[pg_file_test] ✗ FAILED: %s\n", test.name);
    }

    return test_passed;
}

int main(int argc, char **argv) {
    const char *test_dir = "test";

    if (argc > 1) {
        test_dir = argv[1];
    }

    DIR *dir = opendir(test_dir);
    if (!dir) {
        fprintf(stderr, "[pg_file_test] Cannot open test directory: %s\n", test_dir);
        return 1;
    }

    int total = 0;
    int passed = 0;
    int skipped = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) {
            continue;
        }

        /* Check if file ends with .json */
        size_t name_len = strlen(entry->d_name);
        if (name_len < 5 || strcmp(entry->d_name + name_len - 5, ".json") != 0) {
            continue;
        }

        /* Skip non-test JSON files (like all-packages.json, test-no-upgrades.json) */
        if (strncmp(entry->d_name, "01-", 3) != 0 &&
            strncmp(entry->d_name, "02-", 3) != 0 &&
            strncmp(entry->d_name, "03-", 3) != 0 &&
            strncmp(entry->d_name, "04-", 3) != 0 &&
            strncmp(entry->d_name, "05-", 3) != 0 &&
            strncmp(entry->d_name, "06-", 3) != 0 &&
            strncmp(entry->d_name, "07-", 3) != 0 &&
            strncmp(entry->d_name, "08-", 3) != 0 &&
            strncmp(entry->d_name, "09-", 3) != 0) {
            skipped++;
            continue;
        }

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", test_dir, entry->d_name);

        total++;
        if (run_test(filepath)) {
            passed++;
        }
    }

    closedir(dir);

    printf("\n[pg_file_test] Results: %d/%d tests passed", passed, total);
    if (skipped > 0) {
        printf(" (%d files skipped)", skipped);
    }
    printf("\n");

    return (passed == total) ? 0 : 1;
}
