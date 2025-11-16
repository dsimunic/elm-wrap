#ifndef PG_ELM_H
#define PG_ELM_H

#include <stdbool.h>

#include "pg_core.h"
#include "../elm_json.h"
#include "../cache.h"
#include "../registry.h"

struct cJSON;
struct InstallEnv;

/* Elm-specific context used by the PubGrub dependency provider */
typedef struct {
    CacheConfig *cache;
    struct InstallEnv *install_env;  /* For downloading packages */
    bool online;

    int package_count;
    int package_capacity;
    char **authors;
    char **names;

    /* Synthetic root dependencies (derived from elm.json). */
    struct PgElmRootDependency *root_deps;
    size_t root_dep_count;
    size_t root_dep_capacity;

    /* Registry data (from install_env). */
    Registry *registry;
} PgElmContext;

typedef struct PgElmRootDependency {
    PgPackageId pkg;
    PgVersionRange range;
} PgElmRootDependency;

PgElmContext *pg_elm_context_new(struct InstallEnv *install_env, bool online);
void pg_elm_context_free(PgElmContext *ctx);

/* Create a dependency provider wired to Elm's registry and cache. */
PgDependencyProvider pg_elm_make_provider(PgElmContext *ctx);

/* Interning helpers for mapping author/name to PgPackageId. */
PgPackageId pg_elm_root_package_id(void);
PgPackageId pg_elm_intern_package(
    PgElmContext *ctx,
    const char *author,
    const char *name
);

/* Record a root-level dependency constraint for the synthetic project node. */
bool pg_elm_add_root_dependency(
    PgElmContext *ctx,
    PgPackageId pkg,
    PgVersionRange range
);

/* Parse an Elm constraint string like "1.0.0 <= v < 2.0.0" into a version range. */
bool pg_elm_parse_constraint(const char *constraint, PgVersionRange *out_range);

#endif /* PG_ELM_H */
