#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/pgsolver/pg_core.h"
#include "../../src/alloc.h"

/*
 * This file provides a tiny in-memory PgDependencyProvider so we can test the
 * existing solver loop without touching Elm-specific I/O. Each package is
 * identified by a static PgPackageId. Versions are stored newest-first, which
 * matches the contract expected by the solver.
 */

enum {
    PKG_ROOT = 0,
    PKG_ALPHA,
    PKG_BETA,
    PKG_GAMMA,
    PKG_CONFLICT,
    PKG_MISSING
};

#define MAX_PACKAGES 8
#define MAX_VERSIONS 4
#define MAX_DEPS 4

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
    int version_count;
    TestVersionEntry versions[MAX_VERSIONS];
} TestPackageEntry;

typedef struct {
    int package_count;
    TestPackageEntry packages[MAX_PACKAGES];
} TestProviderCtx;

static PgVersion make_version(int major, int minor, int patch) {
    PgVersion v;
    v.major = major;
    v.minor = minor;
    v.patch = patch;
    return v;
}

static void ctx_init(TestProviderCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

static TestPackageEntry *ctx_add_package(TestProviderCtx *ctx, PgPackageId pkg) {
    if (ctx->package_count >= MAX_PACKAGES) {
        return NULL;
    }
    TestPackageEntry *entry = &ctx->packages[ctx->package_count++];
    entry->pkg = pkg;
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

    if (pkg == PKG_ROOT) {
        out_versions[0] = make_version(1, 0, 0);
        return 1;
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

    if (pkg == PKG_ROOT) {
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

static void build_test_context(TestProviderCtx *ctx) {
    ctx_init(ctx);

    /* Alpha depends on Beta ^1.0.0 */
    TestPackageEntry *alpha = ctx_add_package(ctx, PKG_ALPHA);
    TestVersionEntry *alpha_v2 = pkg_add_version(alpha, make_version(2, 0, 0));
    version_add_dependency(
        alpha_v2,
        PKG_BETA,
        pg_range_until_next_major(make_version(1, 0, 0))
    );
    TestVersionEntry *alpha_v1 = pkg_add_version(alpha, make_version(1, 0, 0));
    version_add_dependency(
        alpha_v1,
        PKG_BETA,
        pg_range_until_next_major(make_version(1, 0, 0))
    );

    /* Beta depends on Gamma == 1.0.0 */
    TestPackageEntry *beta = ctx_add_package(ctx, PKG_BETA);
    TestVersionEntry *beta_v11 = pkg_add_version(beta, make_version(1, 1, 0));
    version_add_dependency(
        beta_v11,
        PKG_GAMMA,
        pg_range_exact(make_version(1, 0, 0))
    );
    TestVersionEntry *beta_v10 = pkg_add_version(beta, make_version(1, 0, 0));
    version_add_dependency(
        beta_v10,
        PKG_GAMMA,
        pg_range_exact(make_version(1, 0, 0))
    );

    /* Gamma has a single version with no dependencies */
    TestPackageEntry *gamma = ctx_add_package(ctx, PKG_GAMMA);
    pkg_add_version(gamma, make_version(1, 0, 0));

    /* Conflict package depends on Missing, which has no versions */
    TestPackageEntry *conflict = ctx_add_package(ctx, PKG_CONFLICT);
    TestVersionEntry *conflict_v1 = pkg_add_version(conflict, make_version(1, 0, 0));
    version_add_dependency(
        conflict_v1,
        PKG_MISSING,
        pg_range_exact(make_version(1, 0, 0))
    );

    ctx_add_package(ctx, PKG_MISSING); /* no versions added */
}

static bool expect_selected_version(
    PgSolver *solver,
    PgPackageId pkg,
    PgVersion expected
) {
    PgVersion actual;
    if (!pg_solver_get_selected_version(solver, pkg, &actual)) {
        fprintf(stderr, "[pg_core_test] expected selection for pkg %d\n", pkg);
        return false;
    }
    if (pg_version_compare(actual, expected) != 0) {
        fprintf(
            stderr,
            "[pg_core_test] pkg %d expected %d.%d.%d but got %d.%d.%d\n",
            pkg,
            expected.major,
            expected.minor,
            expected.patch,
            actual.major,
            actual.minor,
            actual.patch
        );
        return false;
    }
    return true;
}

static bool run_basic_resolution_test(TestProviderCtx *ctx) {
    PgDependencyProvider provider = make_test_provider();
    PgSolver *solver = pg_solver_new(
        provider,
        ctx,
        PKG_ROOT,
        make_version(1, 0, 0)
    );
    if (!solver) {
        fprintf(stderr, "[pg_core_test] failed to create solver\n");
        return false;
    }

    bool ok = true;

    if (!pg_solver_add_root_dependency(
            solver, PKG_ALPHA, pg_range_any())) {
        fprintf(stderr, "[pg_core_test] failed to add root dependency\n");
        ok = false;
        goto done;
    }

    PgSolverStatus status = pg_solver_solve(solver);
    if (status != PG_SOLVER_OK) {
        fprintf(stderr, "[pg_core_test] expected OK but solver returned %d\n", status);
        ok = false;
        goto done;
    }

    ok &= expect_selected_version(solver, PKG_ALPHA, make_version(2, 0, 0));
    ok &= expect_selected_version(solver, PKG_BETA, make_version(1, 1, 0));
    ok &= expect_selected_version(solver, PKG_GAMMA, make_version(1, 0, 0));

done:
    pg_solver_free(solver);
    return ok;
}

static bool run_conflict_test(TestProviderCtx *ctx) {
    PgDependencyProvider provider = make_test_provider();
    PgSolver *solver = pg_solver_new(
        provider,
        ctx,
        PKG_ROOT,
        make_version(1, 0, 0)
    );
    if (!solver) {
        fprintf(stderr, "[pg_core_test] failed to create solver\n");
        return false;
    }

    bool ok = true;

    if (!pg_solver_add_root_dependency(
            solver, PKG_CONFLICT, pg_range_any())) {
        fprintf(stderr, "[pg_core_test] failed to add conflict dependency\n");
        ok = false;
        goto done;
    }

    PgSolverStatus status = pg_solver_solve(solver);
    if (status != PG_SOLVER_NO_SOLUTION) {
        fprintf(
            stderr,
            "[pg_core_test] expected NO_SOLUTION but solver returned %d\n",
            status
        );
        ok = false;
    }

done:
    pg_solver_free(solver);
    return ok;
}

int main(void) {
    alloc_init();

    TestProviderCtx ctx;
    build_test_context(&ctx);

    int failures = 0;
    if (!run_basic_resolution_test(&ctx)) {
        failures++;
    }
    if (!run_conflict_test(&ctx)) {
        failures++;
    }

    if (failures == 0) {
        printf("pg_core tests passed\n");
        return 0;
    }

    printf("pg_core tests failed (%d)\n", failures);
    return 1;
}
