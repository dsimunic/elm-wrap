#ifndef RULR_HOST_DEFAULT_H
#define RULR_HOST_DEFAULT_H

#include "rulr_host.h"
#include <stdlib.h>
#include <string.h>

/**
 * Default host using standard library malloc/free/realloc.
 * Use this for simple tools or testing.
 */

static void *rulr_default_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *rulr_default_calloc(void *ctx, size_t count, size_t size) {
    (void)ctx;
    return calloc(count, size);
}

static void *rulr_default_realloc(void *ctx, void *ptr, size_t new_size) {
    (void)ctx;
    return realloc(ptr, new_size);
}

static void rulr_default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

/**
 * Get default host interface using stdlib allocators.
 */
static inline RulrHost rulr_default_host(void) {
    RulrHost host = {
        .arena = {
            .ctx = NULL,
            .malloc = rulr_default_malloc,
            .calloc = rulr_default_calloc,
            .realloc = rulr_default_realloc,
            .free = rulr_default_free,
        }
    };
    return host;
}

#endif /* RULR_HOST_DEFAULT_H */
