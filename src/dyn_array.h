/**
 * dyn_array.h - Dynamic array helper macros
 *
 * Provides macros for safely growing dynamic arrays allocated with arena_malloc.
 * These macros ensure capacity is checked and arrays are reallocated as needed.
 *
 * Usage:
 *   int capacity = 8;
 *   int count = 0;
 *   char **items = arena_malloc(capacity * sizeof(char*));
 *
 *   // Using DYNARRAY_PUSH:
 *   DYNARRAY_PUSH(items, count, capacity, new_item, char*);
 *
 *   // Or manually:
 *   DYNARRAY_ENSURE_CAPACITY(items, count, capacity, char*);
 *   items[count++] = new_item;
 */

#ifndef DYN_ARRAY_H
#define DYN_ARRAY_H

#include "alloc.h"

/**
 * Ensure array has capacity for at least one more element.
 * Doubles capacity if needed, starting from 8 if capacity is 0.
 *
 * @param arr       The array pointer (will be reassigned if reallocated)
 * @param count     Current number of elements
 * @param capacity  Current capacity (will be updated if reallocated)
 * @param elem_type The type of elements in the array
 */
#define DYNARRAY_ENSURE_CAPACITY(arr, count, capacity, elem_type) \
    do { \
        if ((count) >= (capacity)) { \
            (capacity) = (capacity) == 0 ? 8 : (capacity) * 2; \
            (arr) = arena_realloc((arr), (capacity) * sizeof(elem_type)); \
        } \
    } while(0)

/**
 * Push a value onto a dynamic array, growing if needed.
 *
 * @param arr       The array pointer (will be reassigned if reallocated)
 * @param count     Current number of elements (will be incremented)
 * @param capacity  Current capacity (will be updated if reallocated)
 * @param value     The value to push
 * @param elem_type The type of elements in the array
 */
#define DYNARRAY_PUSH(arr, count, capacity, value, elem_type) \
    do { \
        DYNARRAY_ENSURE_CAPACITY(arr, count, capacity, elem_type); \
        (arr)[(count)++] = (value); \
    } while(0)

#endif /* DYN_ARRAY_H */
