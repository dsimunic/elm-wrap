// Simple arena allocator used by wrap.
// This is a standalone implementation, local to this project.

#ifndef LARENA_H
#define LARENA_H

#include <stddef.h>

typedef struct larena_block {
    struct larena_block *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
} larena_block;

typedef struct larena {
    larena_block *head;
    size_t block_size;
} larena;

void larena_init(larena *arena, size_t block_size);
void larena_reset(larena *arena);
void larena_destroy(larena *arena);
void *larena_alloc(larena *arena, size_t size);

#ifdef LARENA_IMPLEMENTATION

#include <stdlib.h>

static size_t larena_align_up(size_t value) {
    const size_t align = sizeof(void *);
    return (value + align - 1) & ~(align - 1);
}

static larena_block *larena_new_block(size_t block_size, size_t min_capacity) {
    size_t capacity = block_size;
    if (capacity < min_capacity) {
        capacity = min_capacity;
    }

    larena_block *block = (larena_block *)malloc(
        sizeof(larena_block) + capacity
    );
    if (!block) {
        return NULL;
    }
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

void larena_init(larena *arena, size_t block_size) {
    if (!arena) {
        return;
    }
    if (block_size == 0) {
        block_size = 1024 * 1024;
    }
    arena->head = NULL;
    arena->block_size = block_size;
}

void larena_reset(larena *arena) {
    if (!arena || !arena->head) {
        return;
    }

    larena_block *block = arena->head->next;
    while (block) {
        larena_block *next = block->next;
        free(block);
        block = next;
    }

    arena->head->next = NULL;
    arena->head->used = 0;
}

void larena_destroy(larena *arena) {
    if (!arena) {
        return;
    }

    larena_block *block = arena->head;
    while (block) {
        larena_block *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

void *larena_alloc(larena *arena, size_t size) {
    if (!arena) {
        return NULL;
    }
    if (size == 0) {
        size = 1;
    }

    size_t aligned_size = larena_align_up(size);

    if (!arena->head ||
        larena_align_up(arena->head->used) + aligned_size > arena->head->capacity) {
        size_t min_capacity = aligned_size;
        larena_block *new_block = larena_new_block(arena->block_size, min_capacity);
        if (!new_block) {
            return NULL;
        }
        new_block->next = arena->head;
        arena->head = new_block;
    }

    larena_block *block = arena->head;
    size_t offset = larena_align_up(block->used);
    if (offset + aligned_size > block->capacity) {
        return NULL;
    }

    void *ptr = block->data + offset;
    block->used = offset + aligned_size;
    return ptr;
}

#endif /* LARENA_IMPLEMENTATION */

#endif /* LARENA_H */

