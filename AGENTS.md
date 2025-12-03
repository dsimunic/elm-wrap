# Agent Guidelines for Memory Management

## Mandatory Reading

Before writing or modifying C code, you **MUST** also read:

- `doc/shared_code_functionality.md` — Shared modules and functions to use when implementing commands

This document describes common utilities (`elm_cmd_common.h`, `elm_project.h`, `fileutil.h`, `rulr/host_helpers.h`) that prevent code duplication and ensure consistency.

---

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

## Dynamic Arrays

When writing code that manages dynamic arrays (arrays that grow as elements are added), **always** follow this pattern:

### Required Pattern for Growing Arrays

```c
// 1. Track both count AND capacity
int items_count = 0;
int items_capacity = 16;  // Use a named constant, not a magic number
Item *items = arena_malloc(items_capacity * sizeof(Item));

// 2. ALWAYS check capacity before adding
if (items_count >= items_capacity) {
    items_capacity *= 2;
    items = arena_realloc(items, items_capacity * sizeof(Item));
}
items[items_count++] = new_item;
```

### ❌ NEVER Do This (Silent Drop Bug)

```c
// WRONG: Data is silently lost when capacity is reached!
if (items_count < items_capacity) {
    items[items_count++] = new_item;
}
// If count >= capacity, the item is simply not added - NO ERROR, NO WARNING
```

### ❌ NEVER Do This (Buffer Overflow)

```c
// WRONG: Fixed allocation with no bounds checking!
char **names = arena_malloc(8 * sizeof(char*));
// Later...
names[names_count++] = new_name;  // CRASH if count > 8
```

### Helper Macro (Optional)

You may use the `DYNARRAY_PUSH` macro from `src/dyn_array.h`:

```c
#include "dyn_array.h"

DYNARRAY_PUSH(items, items_count, items_capacity, new_item, Item);
```

## No Magic Numbers

**Never use literal numbers for array capacities.** Always use named variables or constants.

### ❌ Wrong

```c
char **types = arena_malloc(16 * sizeof(char*));  // What is 16?
// ... 200 lines later ...
types[types_count++] = name;  // Is there still capacity? Who knows!
```

### ✅ Correct

```c
int types_capacity = 16;
char **types = arena_malloc(types_capacity * sizeof(char*));
// Capacity is tracked and can be checked/expanded
```

## Summary

**Golden Rule #1**: In application code, always use `arena_*` functions. Never use standard allocation functions directly.

**Golden Rule #2**: No magic numbers for buffer sizes. Always track capacity in a variable and check before writing.

**No commits**: NEVER commit to Git yourself.

**Building the project**: `make clean all install-user`