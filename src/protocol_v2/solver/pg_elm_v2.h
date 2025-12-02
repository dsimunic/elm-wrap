/**
 * pg_elm_v2.h - V2 Protocol Elm Solver Context
 *
 * This module provides a PubGrub dependency provider that uses the V2
 * registry index. Unlike the V1 provider which reads elm.json files from
 * the cache, this provider has all dependency information directly from
 * the registry index.
 */

#ifndef PROTOCOL_V2_SOLVER_PG_ELM_V2_H
#define PROTOCOL_V2_SOLVER_PG_ELM_V2_H

#include <stdbool.h>
#include "../../pgsolver/pg_core.h"
#include "v2_registry.h"

struct InstallEnv;

/**
 * V2 Elm-specific context used by the PubGrub dependency provider.
 * 
 * This context uses the V2 registry which contains all dependency
 * information, so no cache access is needed for dependency resolution.
 */
typedef struct {
    V2Registry *registry;      /* V2 registry with all package data */
    
    /* Package interning - maps package names to internal IDs */
    int package_count;
    int package_capacity;
    char **authors;
    char **names;
    
    /* Root dependencies from elm.json */
    struct PgElmV2RootDependency *root_deps;
    size_t root_dep_count;
    size_t root_dep_capacity;
} PgElmV2Context;

typedef struct PgElmV2RootDependency {
    PgPackageId pkg;
    PgVersionRange range;
} PgElmV2RootDependency;

/**
 * Create a new V2 Elm context using the given V2 registry.
 *
 * @param registry The V2 registry (ownership is NOT transferred)
 * @return New context, or NULL on error
 */
PgElmV2Context *pg_elm_v2_context_new(V2Registry *registry);

/**
 * Free a V2 Elm context.
 *
 * Note: This does NOT free the registry, as it's owned externally.
 */
void pg_elm_v2_context_free(PgElmV2Context *ctx);

/**
 * Create a dependency provider wired to the V2 registry.
 */
PgDependencyProvider pg_elm_v2_make_provider(PgElmV2Context *ctx);

/**
 * Get the package ID for the synthetic root package.
 */
PgPackageId pg_elm_v2_root_package_id(void);

/**
 * Intern a package name, returning its ID.
 * Creates a new entry if the package is not already interned.
 */
PgPackageId pg_elm_v2_intern_package(
    PgElmV2Context *ctx,
    const char *author,
    const char *name
);

/**
 * Record a root-level dependency constraint.
 */
bool pg_elm_v2_add_root_dependency(
    PgElmV2Context *ctx,
    PgPackageId pkg,
    PgVersionRange range
);

/**
 * Parse an Elm constraint string like "1.0.0 <= v < 2.0.0" into a version range.
 */
bool pg_elm_v2_parse_constraint(const char *constraint, PgVersionRange *out_range);

#endif /* PROTOCOL_V2_SOLVER_PG_ELM_V2_H */
