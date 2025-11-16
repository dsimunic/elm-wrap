# Agent Guidelines for Memory Management

## Memory Allocation Rules

This codebase uses a custom arena allocator (`larena.h`) for all memory management. **All application code must use the wrapper functions** defined in `alloc.h`.

### Required Functions

When writing or modifying C code, **always** use these functions:

```c
void* arena_malloc(size_t size);
void* arena_calloc(size_t count, size_t size);
void* arena_realloc(void *ptr, size_t size);
char* arena_strdup(const char *s);
void arena_free(void *ptr);
```

### Forbidden Functions

**NEVER** use these standard library functions directly in application code:

- ❌ `malloc()`
- ❌ `calloc()`
- ❌ `realloc()`
- ❌ `strdup()`
- ❌ `free()`

### Examples

#### ✅ Correct Usage

```c
// Allocate memory
char *buffer = arena_malloc(256);
if (!buffer) {
    return ERROR_OUT_OF_MEMORY;
}

// Duplicate a string
char *copy = arena_strdup(original);

// Reallocate array
items = arena_realloc(items, new_count * sizeof(Item));

// Free memory (arena handles this, but call for consistency)
arena_free(buffer);
```

#### ❌ Incorrect Usage

```c
// DON'T DO THIS - bypasses arena allocator
char *buffer = malloc(256);  // WRONG
char *copy = strdup(str);     // WRONG
free(buffer);                 // WRONG
```

## Arena Allocator Benefits

The arena allocator (`larena.h`) provides:

1. **Automatic cleanup** - Call `larena_destroy()` to free all allocations at once
2. **Performance** - Faster allocation with reduced fragmentation
3. **Safety** - Prevents memory leaks from forgotten `free()` calls
4. **Simplicity** - No need to track individual allocations

## Implementation Details

- **alloc.c**: Implements the `arena_*` functions using `larena.h`
- **alloc.h**: Declares the wrapper API (include this in your code)
- **larena.h**: Arena allocator implementation (uses system `malloc`/`free` internally)

## Required Headers

Always include at the top of C source files:

```c
#include "alloc.h"
```

This provides access to all `arena_*` functions.

## Exception

The **only** file that may use `malloc()`/`free()` directly is `src/larena.h`, as it **is** the allocator implementation itself.

## Verification

Before committing code, verify no direct allocations exist:

```bash
# Should return no results
grep -r --include="*.c" '\bmalloc\s*(' src/ | grep -v arena_malloc | grep -v internal_malloc
grep -r --include="*.c" '\bcalloc\s*(' src/ | grep -v arena_calloc
grep -r --include="*.c" '\brealloc\s*(' src/ | grep -v arena_realloc
grep -r --include="*.c" '\bstrdup\s*(' src/ | grep -v arena_strdup
```

## Summary

**Golden Rule**: In application code, always use `arena_*` functions. Never use standard allocation functions directly.
