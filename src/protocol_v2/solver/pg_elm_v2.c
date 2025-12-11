/**
 * pg_elm_v2.c - V2 Protocol Elm Solver Context Implementation
 *
 * Implements the PubGrub dependency provider using V2 registry data.
 * The key difference from V1 is that all dependency information is available
 * directly in the registry index, so no cache access is needed.
 */

#include "pg_elm_v2.h"
#include "v2_registry.h"
#include "../../pgsolver/solver_common.h"
#include "../../alloc.h"
#include "../../log.h"
#include "../../commands/package/package_common.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

PgElmV2Context *pg_elm_v2_context_new(V2Registry *registry) {
    if (!registry) {
        log_error("pg_elm_v2_context_new: NULL registry");
        return NULL;
    }
    
    PgElmV2Context *ctx = (PgElmV2Context *)arena_malloc(sizeof(PgElmV2Context));
    if (!ctx) {
        return NULL;
    }
    
    ctx->registry = registry;
    
    ctx->package_capacity = 8;
    ctx->package_count = 1; /* Reserve id 0 for root */
    ctx->authors = (char **)arena_calloc((size_t)ctx->package_capacity, sizeof(char *));
    ctx->names = (char **)arena_calloc((size_t)ctx->package_capacity, sizeof(char *));
    if (!ctx->authors || !ctx->names) {
        arena_free(ctx->authors);
        arena_free(ctx->names);
        arena_free(ctx);
        return NULL;
    }
    
    ctx->authors[0] = arena_strdup("__root__");
    ctx->names[0] = arena_strdup("__root__");
    ctx->root_deps = NULL;
    ctx->root_dep_count = 0;
    ctx->root_dep_capacity = 0;
    
    return ctx;
}

void pg_elm_v2_context_free(PgElmV2Context *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->authors) {
        for (int i = 0; i < ctx->package_count; i++) {
            arena_free(ctx->authors[i]);
        }
        arena_free(ctx->authors);
    }
    if (ctx->names) {
        for (int i = 0; i < ctx->package_count; i++) {
            arena_free(ctx->names[i]);
        }
        arena_free(ctx->names);
    }
    
    arena_free(ctx->root_deps);
    arena_free(ctx);
}

PgPackageId pg_elm_v2_root_package_id(void) {
    /* Reserve package id 0 for the synthetic root */
    return 0;
}

PgPackageId pg_elm_v2_intern_package(
    PgElmV2Context *ctx,
    const char *author,
    const char *name
) {
    if (!ctx || !author || !name) {
        return -1;
    }
    
    /* Look for existing entry */
    for (int i = 0; i < ctx->package_count; i++) {
        if (ctx->authors[i] && strcmp(ctx->authors[i], author) == 0 &&
            ctx->names[i] && strcmp(ctx->names[i], name) == 0) {
            return (PgPackageId)i;
        }
    }
    
    /* Need a new entry */
    if (ctx->package_count >= ctx->package_capacity) {
        int new_capacity = ctx->package_capacity * 2;
        char **new_authors = (char **)arena_realloc(
            ctx->authors, (size_t)new_capacity * sizeof(char *));
        char **new_names = (char **)arena_realloc(
            ctx->names, (size_t)new_capacity * sizeof(char *));
        if (!new_authors || !new_names) {
            arena_free(new_authors);
            arena_free(new_names);
            return -1;
        }
        ctx->authors = new_authors;
        ctx->names = new_names;
        for (int i = ctx->package_capacity; i < new_capacity; i++) {
            ctx->authors[i] = NULL;
            ctx->names[i] = NULL;
        }
        ctx->package_capacity = new_capacity;
    }
    
    int id = ctx->package_count;
    ctx->authors[id] = arena_strdup(author);
    ctx->names[id] = arena_strdup(name);
    if (!ctx->authors[id] || !ctx->names[id]) {
        arena_free(ctx->authors[id]);
        arena_free(ctx->names[id]);
        ctx->authors[id] = NULL;
        ctx->names[id] = NULL;
        return -1;
    }
    
    ctx->package_count++;
    return (PgPackageId)id;
}

static bool pg_elm_v2_get_author_name(
    PgElmV2Context *ctx,
    PgPackageId pkg,
    const char **out_author,
    const char **out_name
) {
    if (!ctx || pkg < 0 || pkg >= ctx->package_count) {
        return false;
    }
    *out_author = ctx->authors[pkg];
    *out_name = ctx->names[pkg];
    return (*out_author != NULL && *out_name != NULL);
}

static int pg_elm_v2_provider_get_versions(
    void *ctx_ptr,
    PgPackageId pkg,
    PgVersion *out_versions,
    size_t out_capacity
) {
    PgElmV2Context *ctx = (PgElmV2Context *)ctx_ptr;
    
    if (!ctx || !out_versions || out_capacity == 0) {
        return 0;
    }
    
    /* Root package has a single synthetic version */
    if (pkg == pg_elm_v2_root_package_id()) {
        PgVersion v;
        v.major = 1;
        v.minor = 0;
        v.patch = 0;
        out_versions[0] = v;
        return 1;
    }
    
    const char *author = NULL;
    const char *name = NULL;
    if (!pg_elm_v2_get_author_name(ctx, pkg, &author, &name)) {
        return 0;
    }
    
    if (!ctx->registry) {
        return 0;
    }
    
    /* Find the package in the V2 registry */
    V2PackageEntry *entry = v2_registry_find(ctx->registry, author, name);
    if (!entry) {
        log_trace("Package %s/%s not found in V2 registry", author, name);
        return 0;
    }
    
    log_trace("Found %s/%s in V2 registry with %zu versions", author, name, entry->version_count);
    
    /* Copy versions to output buffer (V2 registry stores newest-first) */
    int written = 0;
    for (size_t i = 0; i < entry->version_count && written < (int)out_capacity; i++) {
        V2PackageVersion *pv = &entry->versions[i];
        
        /* Skip non-valid versions */
        if (pv->status != V2_STATUS_VALID) {
            continue;
        }
        
        PgVersion v;
        v.major = pv->major;
        v.minor = pv->minor;
        v.patch = pv->patch;
        log_trace("  Version %zu: %d.%d.%d", i, v.major, v.minor, v.patch);
        out_versions[written] = v;
        written++;
    }
    
    log_trace("Returning %d versions for %s/%s", written, author, name);
    return written;
}

bool pg_elm_v2_add_root_dependency(
    PgElmV2Context *ctx,
    PgPackageId pkg,
    PgVersionRange range
) {
    if (!ctx || pkg < 0 || range.is_empty) {
        return false;
    }
    
    if (ctx->root_dep_count >= ctx->root_dep_capacity) {
        size_t new_capacity = ctx->root_dep_capacity == 0
            ? 8
            : ctx->root_dep_capacity * 2;
        PgElmV2RootDependency *new_items = (PgElmV2RootDependency *)arena_realloc(
            ctx->root_deps,
            new_capacity * sizeof(PgElmV2RootDependency)
        );
        if (!new_items) {
            return false;
        }
        ctx->root_deps = new_items;
        ctx->root_dep_capacity = new_capacity;
    }
    
    ctx->root_deps[ctx->root_dep_count].pkg = pkg;
    ctx->root_deps[ctx->root_dep_count].range = range;
    ctx->root_dep_count++;
    return true;
}

static int pg_elm_v2_provider_get_dependencies(
    void *ctx_ptr,
    PgPackageId pkg,
    PgVersion version,
    PgPackageId *out_pkgs,
    PgVersionRange *out_ranges,
    size_t out_capacity
) {
    PgElmV2Context *ctx = (PgElmV2Context *)ctx_ptr;
    
    if (!ctx || !out_pkgs || !out_ranges || out_capacity == 0) {
        return 0;
    }
    
    if (pkg == pg_elm_v2_root_package_id()) {
        size_t available = ctx->root_dep_count;
        if (available > out_capacity) {
            available = out_capacity;
        }
        for (size_t i = 0; i < available; i++) {
            out_pkgs[i] = ctx->root_deps[i].pkg;
            out_ranges[i] = ctx->root_deps[i].range;
        }
        return (int)available;
    }
    
    const char *author = NULL;
    const char *name = NULL;
    if (!pg_elm_v2_get_author_name(ctx, pkg, &author, &name)) {
        return 0;
    }
    
    /* Find the specific version in the V2 registry */
    V2PackageVersion *pv = v2_registry_find_version(
        ctx->registry, author, name,
        (uint16_t)version.major, (uint16_t)version.minor, (uint16_t)version.patch
    );
    
    if (!pv) {
        log_trace("Version %d.%d.%d not found for %s/%s in V2 registry",
                  version.major, version.minor, version.patch, author, name);
        return 0;
    }
    
    /* Read dependencies directly from registry (no cache access needed!) */
    int written = 0;
    for (size_t i = 0; i < pv->dependency_count && written < (int)out_capacity; i++) {
        V2Dependency *dep = &pv->dependencies[i];

        /* Parse package name (author/name format) */
        char *dep_author = NULL;
        char *dep_name = NULL;
        if (!parse_package_name(dep->package_name, &dep_author, &dep_name)) {
            continue;
        }
        
        /* Parse constraint */
        PgVersionRange range;
        if (!version_parse_constraint(dep->constraint, &range)) {
            arena_free(dep_author);
            continue;
        }
        
        /* Intern the dependency package */
        PgPackageId dep_id = pg_elm_v2_intern_package(ctx, dep_author, dep_name);
        arena_free(dep_author);
        
        if (dep_id < 0) {
            continue;
        }
        
        out_pkgs[written] = dep_id;
        out_ranges[written] = range;
        written++;
    }
    
    return written;
}

const char *pg_elm_v2_get_package_name(PgElmV2Context *ctx, PgPackageId pkg) {
    if (!ctx || pkg < 0 || pkg >= ctx->package_count) {
        return NULL;
    }

    /* For root package, return the special name */
    if (pkg == 0) {
        return "__root__";
    }

    /* Build "author/name" string */
    const char *author = ctx->authors[pkg];
    const char *name = ctx->names[pkg];

    if (!author || !name) {
        return NULL;
    }

    /* Allocate and build the full package name */
    size_t len = strlen(author) + 1 + strlen(name) + 1;
    char *full_name = arena_malloc(len);
    if (!full_name) {
        return NULL;
    }

    snprintf(full_name, len, "%s/%s", author, name);
    return full_name;
}

const char *pg_elm_v2_get_package_name_with_ctx(void *ctx, PgPackageId pkg) {
    if (!ctx) {
        return NULL;
    }
    PgExplainContext *explain_ctx = (PgExplainContext *)ctx;
    PgElmV2Context *pg_ctx = (PgElmV2Context *)explain_ctx->resolver_ctx;
    return pg_elm_v2_get_package_name(pg_ctx, pkg);
}

PgDependencyProvider pg_elm_v2_make_provider(PgElmV2Context *ctx) {
    PgDependencyProvider provider;
    provider.get_versions = pg_elm_v2_provider_get_versions;
    provider.get_dependencies = pg_elm_v2_provider_get_dependencies;
    (void)ctx;
    return provider;
}
