#include "../install_env.h"
#include "../registry.h"
#include "pg_elm.h"
#include "../alloc.h"
#include "../log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

PgElmContext *pg_elm_context_new(struct InstallEnv *install_env, bool online) {
    PgElmContext *ctx = (PgElmContext *)arena_malloc(sizeof(PgElmContext));
    if (!ctx) {
        return NULL;
    }
    ctx->install_env = install_env;
    ctx->cache = install_env ? install_env->cache : NULL;
    ctx->registry = install_env ? install_env->registry : NULL;
    ctx->online = online;

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

void pg_elm_context_free(PgElmContext *ctx) {
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

PgPackageId pg_elm_root_package_id(void) {
    /* Reserve package id 0 for the synthetic root. */
    return 0;
}

PgPackageId pg_elm_intern_package(
    PgElmContext *ctx,
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

static bool pg_elm_get_author_name(
    PgElmContext *ctx,
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

static int pg_elm_provider_get_versions(
    void *ctx_ptr,
    PgPackageId pkg,
    PgVersion *out_versions,
    size_t out_capacity
) {
    PgElmContext *ctx = (PgElmContext *)ctx_ptr;

    if (!ctx || !out_versions || out_capacity == 0) {
        return 0;
    }

    /* Root package has a single synthetic version. */
    if (pkg == pg_elm_root_package_id()) {
        PgVersion v;
        v.major = 1;
        v.minor = 0;
        v.patch = 0;
        out_versions[0] = v;
        return 1;
    }

    const char *author = NULL;
    const char *name = NULL;
    if (!pg_elm_get_author_name(ctx, pkg, &author, &name)) {
        return 0;
    }

    if (!ctx->registry) {
        return 0;
    }

    /* Find the package in the registry */
    RegistryEntry *entry = registry_find(ctx->registry, author, name);
    if (!entry) {
        return 0;
    }

    log_debug("Found %s/%s in registry with %zu versions", author, name, entry->version_count);

    /* Copy versions to output buffer (registry stores newest-first, which is what we want) */
    int written = 0;
    for (size_t i = 0; i < entry->version_count && written < (int)out_capacity; i++) {
        PgVersion v;
        v.major = entry->versions[i].major;
        v.minor = entry->versions[i].minor;
        v.patch = entry->versions[i].patch;
        log_debug("  Version %zu: %d.%d.%d", i, v.major, v.minor, v.patch);
        out_versions[written] = v;
        written++;
    }

    log_debug("Returning %d versions for %s/%s", written, author, name);
    return written;
}

bool pg_elm_parse_constraint(
    const char *constraint,
    PgVersionRange *out_range
) {
    if (!constraint || !out_range) {
        return false;
    }

    /* Elm's package constraints look like: "1.0.0 <= v < 2.0.0" */
    int a1, a2, a3;
    int b1, b2, b3;

    int matched = sscanf(
        constraint,
        " %d.%d.%d <= v < %d.%d.%d",
        &a1, &a2, &a3,
        &b1, &b2, &b3
    );

    if (matched != 6) {
        return false;
    }

    PgVersion lower_v;
    lower_v.major = a1;
    lower_v.minor = a2;
    lower_v.patch = a3;

    PgVersion upper_v;
    upper_v.major = b1;
    upper_v.minor = b2;
    upper_v.patch = b3;

    PgVersionRange r;
    r.lower.v = lower_v;
    r.lower.inclusive = true;
    r.lower.unbounded = false;

    r.upper.v = upper_v;
    r.upper.inclusive = false;
    r.upper.unbounded = false;

    r.is_empty = false;
    *out_range = r;
    return true;
}

bool pg_elm_add_root_dependency(
    PgElmContext *ctx,
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
        PgElmRootDependency *new_items = (PgElmRootDependency *)arena_realloc(
            ctx->root_deps,
            new_capacity * sizeof(PgElmRootDependency)
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

static int pg_elm_provider_get_dependencies(
    void *ctx_ptr,
    PgPackageId pkg,
    PgVersion version,
    PgPackageId *out_pkgs,
    PgVersionRange *out_ranges,
    size_t out_capacity
) {
    PgElmContext *ctx = (PgElmContext *)ctx_ptr;

    if (!ctx || !out_pkgs || !out_ranges || out_capacity == 0) {
        return 0;
    }

    if (pkg == pg_elm_root_package_id()) {
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
    if (!pg_elm_get_author_name(ctx, pkg, &author, &name)) {
        return 0;
    }

    char version_str[64];
    snprintf(
        version_str,
        sizeof(version_str),
        "%d.%d.%d",
        version.major,
        version.minor,
        version.patch
    );

    char *pkg_path = cache_get_package_path(ctx->cache, author, name, version_str);
    if (!pkg_path) {
        return 0;
    }

    char elm_json_path[1024];
    snprintf(elm_json_path, sizeof(elm_json_path), "%s/elm.json", pkg_path);
    arena_free(pkg_path);

    ElmJson *elm_json = elm_json_read(elm_json_path);
    if (!elm_json) {
        if (ctx->online && ctx->install_env) {
            /* Attempt to download and retry once */
            if (cache_download_package_with_env(ctx->install_env, author, name, version_str)) {
                elm_json = elm_json_read(elm_json_path);
            }
        }
    }

    if (!elm_json) {
        fprintf(stderr,
                "[PgElm] Failed to load elm.json for %s/%s@%s\n",
                author, name, version_str);
        return 0;
    }

    int written = 0;

    if (elm_json->type == ELM_PROJECT_PACKAGE &&
        elm_json->package_dependencies) {

        PackageMap *deps = elm_json->package_dependencies;
        for (int i = 0; i < deps->count && written < (int)out_capacity; i++) {
            Package *p = &deps->packages[i];
            PgVersionRange range;
            if (!pg_elm_parse_constraint(p->version, &range)) {
                continue;
            }

            PgPackageId dep_id = pg_elm_intern_package(ctx, p->author, p->name);
            if (dep_id < 0) {
                continue;
            }

            out_pkgs[written] = dep_id;
            out_ranges[written] = range;
            written++;
        }
    }

    elm_json_free(elm_json);
    return written;
}

PgDependencyProvider pg_elm_make_provider(PgElmContext *ctx) {
    PgDependencyProvider provider;
    provider.get_versions = pg_elm_provider_get_versions;
    provider.get_dependencies = pg_elm_provider_get_dependencies;
    (void)ctx;
    return provider;
}
