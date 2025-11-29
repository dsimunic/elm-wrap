#ifndef MINI_DATALOG_DYN_ARRAY_H
#define MINI_DATALOG_DYN_ARRAY_H

#include <stddef.h>
#include "alloc.h"

#define DYNARRAY_INIT_CAPACITY 16

/*
 * Grow an arena-backed array to at least `needed` elements.
 * Returns 0 on success, -1 on allocation failure.
 */
static inline int dynarray_reserve(void **arr, int *capacity, size_t elem_size, int needed) {
    if (*capacity >= needed) {
        return 0;
    }
    size_t new_cap = *capacity == 0 ? DYNARRAY_INIT_CAPACITY : (size_t)(*capacity) * 2;
    while ((int)new_cap < needed) {
        new_cap *= 2;
    }
    void *new_mem = arena_realloc(*arr, elem_size * new_cap);
    if (new_mem == NULL) {
        return -1;
    }
    *arr = new_mem;
    *capacity = (int)new_cap;
    return 0;
}

/*
 * Append helper that expands the buffer and writes the value.
 * The caller must supply the pointer variable (modifiable), count, and capacity variables.
 * Returns 0 on success, -1 on allocation failure.
 */
#define DYNARRAY_PUSH(arr_ptr, count_var, cap_var, value)                     \
    (((dynarray_reserve((void **)&(arr_ptr), &(cap_var),                      \
                        sizeof(*(arr_ptr)), (count_var) + 1) == 0) &&        \
      (((arr_ptr)[(count_var)] = (value)), ++(count_var), 1))                \
         ? 0                                                                 \
         : -1)

#endif /* MINI_DATALOG_DYN_ARRAY_H */
