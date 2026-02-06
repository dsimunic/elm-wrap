#ifndef ELM_WRAP_RULR_COMPAT_H
#define ELM_WRAP_RULR_COMPAT_H

/*
 * rulr_compat.h
 *
 * Compatibility layer for integrating multiple librulr API generations.
 *
 * Older vendored headers expose:
 *   RulrError rulr_init(Rulr *r);
 *
 * Newer (host-controlled) headers expose:
 *   RulrError rulr_init(Rulr *r, const RulrHost *host);
 *
 * This header provides wrap_rulr_init() which compiles against either API.
 */

#include "rulr.h"

/* Detect whether the host-based API is available in the current vendored headers. */
#if defined(RULR_HOST_H)
#define WRAP_RULR_HAS_HOST_API 1
#elif defined(__has_include)
#if __has_include("rulr_host.h")
#include "rulr_host.h"
#define WRAP_RULR_HAS_HOST_API 1
#else
#define WRAP_RULR_HAS_HOST_API 0
#endif
#else
#define WRAP_RULR_HAS_HOST_API 0
#endif

#if WRAP_RULR_HAS_HOST_API
#include "../alloc.h"

static inline void *wrap_rulr_arena_malloc_cb(void *ctx, size_t size) {
    (void)ctx;
    return arena_malloc(size);
}

static inline void *wrap_rulr_arena_calloc_cb(void *ctx, size_t count, size_t size) {
    (void)ctx;
    return arena_calloc(count, size);
}

static inline void *wrap_rulr_arena_realloc_cb(void *ctx, void *ptr, size_t new_size) {
    (void)ctx;
    return arena_realloc(ptr, new_size);
}

static inline void wrap_rulr_arena_free_cb(void *ctx, void *ptr) {
    (void)ctx;
    arena_free(ptr);
}

static inline RulrHost wrap_rulr_arena_host(void) {
    RulrHost host = {
        .arena = {
            .ctx = NULL,
            .malloc = wrap_rulr_arena_malloc_cb,
            .calloc = wrap_rulr_arena_calloc_cb,
            .realloc = wrap_rulr_arena_realloc_cb,
            .free = wrap_rulr_arena_free_cb,
        },
        .managed = NULL,
    };
    return host;
}
#endif

static inline RulrError wrap_rulr_init(Rulr *r) {
#if WRAP_RULR_HAS_HOST_API
    RulrHost host = wrap_rulr_arena_host();
    return rulr_init(r, &host);
#else
    return rulr_init(r);
#endif
}

#endif /* ELM_WRAP_RULR_COMPAT_H */
