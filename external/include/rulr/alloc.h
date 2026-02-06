#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>

struct RulrHost;

/**
 * Initialize the global arena allocator with host-provided memory callbacks.
 * Must be called before any arena_* functions.
 *
 * @param host  Host interface with allocation callbacks
 */
void alloc_init_with_host(const struct RulrHost *host);

/**
 * Initialize the global arena allocator.
 * Safe to call multiple times (idempotent).
 * NOTE: alloc_init_with_host() must be called first.
 */
void alloc_init(void);

/**
 * Shutdown and free all arena memory.
 */
void alloc_shutdown(void);

/**
 * Arena-backed allocation functions.
 * Memory is bulk-freed on alloc_shutdown().
 */
void *arena_malloc(size_t size);
void *arena_calloc(size_t count, size_t size);
void *arena_realloc(void *ptr, size_t size);
char *arena_strdup(const char *s);
void  arena_free(void *ptr);

/**
 * Arena allocation statistics.
 */
typedef struct {
    size_t total_allocated;  /* Total bytes allocated from system */
    size_t total_used;       /* Total bytes used by allocations */
    size_t block_count;      /* Number of arena blocks */
} AllocStats;

/**
 * Get current arena allocation statistics.
 */
void alloc_get_stats(AllocStats *stats);

#endif /* ALLOC_H */
