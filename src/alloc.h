#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>

/* Global arena-backed allocation API */
void alloc_init(void);
void alloc_shutdown(void);

void *arena_malloc(size_t size);
void *arena_calloc(size_t count, size_t size);
void *arena_realloc(void *ptr, size_t size);
char *arena_strdup(const char *s);
void arena_free(void *ptr);

/* Workaround for librulr uninitialized stack memory. See alloc.c */
void rulr_stack_sanitize(int depth);

#endif /* ALLOC_H */

