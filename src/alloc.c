#define LARENA_IMPLEMENTATION
#include "larena.h"
#include "alloc.h"
#include "constants.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

static larena global_arena;
static bool alloc_initialized = false;

static void alloc_ensure_init(void) {
    if (!alloc_initialized) {
        larena_init(&global_arena, INITIAL_ARENA_SIZE);
        alloc_initialized = true;
    }
}

void alloc_init(void) {
    alloc_ensure_init();
}

void alloc_shutdown(void) {
    if (!alloc_initialized) {
        return;
    }
    larena_destroy(&global_arena);
    alloc_initialized = false;
}

static void *arena_alloc_with_header(size_t size, bool zero) {
    alloc_ensure_init();

    if (size == 0) {
        size = 1;
    }

    if (SIZE_MAX - size < sizeof(size_t)) {
        return NULL;
    }
    size_t total = sizeof(size_t) + size;

    size_t *base = (size_t *)larena_alloc(&global_arena, total);
    if (!base) {
        return NULL;
    }

    base[0] = size;
    void *ptr = (void *)(base + 1);
    if (zero) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *arena_malloc(size_t size) {
    return arena_alloc_with_header(size, false);
}

void *arena_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) {
        return arena_alloc_with_header(0, true);
    }
    if (SIZE_MAX / count < size) {
        return NULL;
    }
    size_t total = count * size;
    return arena_alloc_with_header(total, true);
}

void *arena_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return arena_malloc(size);
    }

    if (size == 0) {
        arena_free(ptr);
        return NULL;
    }

    size_t old_size = ((size_t *)ptr)[-1];

    if (size <= old_size) {
        ((size_t *)ptr)[-1] = size;
        return ptr;
    }

    void *new_ptr = arena_malloc(size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size);
    return new_ptr;
}

char *arena_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *copy = (char *)arena_malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

void arena_free(void *ptr) {
    (void)ptr;
}

/*
 * Workaround for uninitialized IterState.extern_buf in librulr.
 * This function recursively zeros stack memory at a configurable depth.
 * Call this before rulr_evaluate() to zero the stack area that will be
 * reused by execute_plan_rule's PlanExecCtx structure.
 *
 * The depth parameter controls how deep to recurse (to match rulr's internal
 * call depth). The 16KB buffer size is chosen to cover PlanExecCtx.iters[].
 */
__attribute__((noinline))
void rulr_stack_sanitize(int depth) {
    volatile char buf[16384];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = 0;
    }
    if (depth > 0) {
        rulr_stack_sanitize(depth - 1);
    }
}

