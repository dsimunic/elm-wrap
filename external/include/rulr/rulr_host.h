#ifndef RULR_HOST_H
#define RULR_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Host-provided memory allocation interface.
 *
 * Hosts provide their own allocator by passing a RulrHost struct to rulr_init().
 * All allocations within the Rulr instance use these callbacks.
 *
 * The ctx pointer is passed to all callbacks, allowing hosts to track
 * allocations or use instance-specific allocators.
 */
typedef struct RulrArenaApi {
    void *ctx;  /* Host-owned context (opaque to librulr) */
    void *(*malloc)(void *ctx, size_t size);
    void *(*calloc)(void *ctx, size_t count, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t new_size);
    void  (*free)(void *ctx, void *ptr);
} RulrArenaApi;

/**
 * Managed heap API for GC/RC integration.
 *
 * Hosts that use a tracing or reference-counting GC implement this interface
 * to allow librulr-allocated objects to participate in managed memory.
 * When NULL, librulr operates in arena-only mode.
 */
typedef struct RulrManagedApi {
    void *ctx;  /* Host-owned context, opaque to librulr */

    void *(*alloc_object)(void *ctx, uint32_t kind, size_t bytes, size_t alignment);
    void *(*alloc_blob)(void *ctx, size_t bytes);
    uintptr_t *(*alloc_ptr_array)(void *ctx, size_t count);
    void (*inc)(void *ctx, uintptr_t ref);
    void (*dec)(void *ctx, uintptr_t ref);
    void (*safepoint)(void *ctx, uint32_t reason_bits);
    void (*write_barrier)(void *ctx, void *parent, void *child_ptr);
    bool (*register_roots)(void *ctx, void *enumerator_fn, void *enum_ctx, const char *name);
} RulrManagedApi;

/**
 * Host interface for librulr.
 *
 * Contains arena allocation and optional managed heap API for GC/RC integration.
 *
 * NOTE: librulr.a must be rebuilt from source when this header changes,
 * as the Rulr struct stores RulrHost by value.
 */
typedef struct RulrHost {
    RulrArenaApi arena;
    const RulrManagedApi *managed;  /* NULL = arena-only mode */
} RulrHost;

#endif /* RULR_HOST_H */
