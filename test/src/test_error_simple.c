#include <stdio.h>
#include "../../src/pgsolver/pg_core.h"
#include "../../src/alloc.h"

/* Simple test for error reporting */

static int test_get_versions(void *ctx, PgPackageId pkg, PgVersion *out, size_t cap) {
    /* pkg 0 = foo: 1.0.0 */
    /* pkg 1 = bar: 2.0.0 */
    /* pkg 2 = baz: 1.0.0, 3.0.0 */
    if (pkg == 0 && cap > 0) {
        out[0].major = 1; out[0].minor = 0; out[0].patch = 0;
        return 1;
    }
    if (pkg == 1 && cap > 0) {
        out[0].major = 2; out[0].minor = 0; out[0].patch = 0;
        return 1;
    }
    if (pkg == 2 && cap >= 2) {
        out[0].major = 3; out[0].minor = 0; out[0].patch = 0;
        out[1].major = 1; out[1].minor = 0; out[1].patch = 0;
        return 2;
    }
    return 0;
}

static int test_get_dependencies(
    void *ctx,
    PgPackageId pkg,
    PgVersion version,
    PgPackageId *out_pkgs,
    PgVersionRange *out_ranges,
    size_t cap
) {
    /* foo 1.0.0 depends on bar ^2.0.0 */
    if (pkg == 0 && cap > 0) {
        out_pkgs[0] = 1;
        out_ranges[0] = pg_range_until_next_major((PgVersion){2, 0, 0});
        return 1;
    }
    /* bar 2.0.0 depends on baz ^3.0.0 */
    if (pkg == 1 && cap > 0) {
        out_pkgs[0] = 2;
        out_ranges[0] = pg_range_until_next_major((PgVersion){3, 0, 0});
        return 1;
    }
    /* baz has no dependencies */
    return 0;
}

static const char *test_name_resolver(void *ctx, PgPackageId pkg) {
    switch (pkg) {
        case 0: return "foo";
        case 1: return "bar";
        case 2: return "baz";
        case 999: return "root";
        default: return "<unknown>";
    }
}

int main(void) {
    alloc_init();

    PgDependencyProvider provider;
    provider.get_versions = test_get_versions;
    provider.get_dependencies = test_get_dependencies;

    PgSolver *solver = pg_solver_new(provider, NULL, 999, (PgVersion){1, 0, 0});
    if (!solver) {
        fprintf(stderr, "Failed to create solver\n");
        return 1;
    }

    /* root depends on foo ^1.0.0 and baz ^1.0.0 */
    pg_solver_add_root_dependency(solver, 0, pg_range_until_next_major((PgVersion){1, 0, 0}));
    pg_solver_add_root_dependency(solver, 2, pg_range_until_next_major((PgVersion){1, 0, 0}));

    PgSolverStatus status = pg_solver_solve(solver);

    if (status == PG_SOLVER_NO_SOLUTION) {
        char error_msg[4096];
        if (pg_solver_explain_failure(solver, test_name_resolver, NULL,
                                       error_msg, sizeof(error_msg))) {
            printf("\n%s\n", error_msg);
        } else {
            printf("Failed to generate error message\n");
        }
    } else {
        printf("Unexpected status: %d\n", status);
    }

    pg_solver_free(solver);
    return 0;
}
