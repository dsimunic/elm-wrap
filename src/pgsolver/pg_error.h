#ifndef PG_ERROR_H
#define PG_ERROR_H

#include "pg_core.h"

/*
 * Error reporting for PubGrub solver failures.
 *
 * When the solver determines no solution exists, it produces a root
 * incompatibility that proves version solving has failed. This module
 * traverses the derivation graph for that incompatibility and generates
 * a human-readable explanation.
 */

/* Callback for resolving package IDs to human-readable names */
typedef const char *(*PgPackageNameResolver)(void *ctx, PgPackageId pkg);

/*
 * Generate a human-readable error message explaining why solving failed.
 *
 * solver: The solver that failed to find a solution
 * root_incompatibility: The incompatibility that proves failure
 * name_resolver: Callback to convert package IDs to names
 * name_ctx: Context passed to name_resolver
 * out_buffer: Buffer to write the error message
 * buffer_size: Size of out_buffer
 *
 * Returns true on success, false on error (e.g., buffer too small)
 */
bool pg_error_report(
    void *solver_ptr,
    void *root_incompatibility_ptr,
    PgPackageNameResolver name_resolver,
    void *name_ctx,
    char *out_buffer,
    size_t buffer_size
);

#endif /* PG_ERROR_H */
