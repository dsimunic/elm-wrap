# Agent Guidelines for Memory Management

## Project and binary name

We refer to the project as elm-wrap and the main binary as `wrap`. In documentation and code comments, always use these names. In markdown, always use emphasis for the project name: **elm-wrap** and backticks for the binary name: `wrap`.

## Mandatory Reading

Before writing or modifying C code, you **MUST** also read:

- `doc/writing-secure-code.md` — Secure coding rules (edicts) for humans and coding LLMs
- `doc/shared_code_functionality.md` — Shared modules and functions to use when implementing commands
- `doc/global_context.md` — Global context all commands operate in.

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

---

## Using Constants Instead of Magic Numbers

The codebase provides centralized constants in `src/constants.h` to eliminate magic numbers. **ALWAYS** use these constants instead of hard-coded numbers.

### Required Header

Include at the top of source files (use correct path based on file location):

```c
#include "constants.h"          // From src/ directory
#include "../constants.h"       // From src/subdirectory
#include "../../constants.h"    // From src/subdir/subdir
```

### Available Constants

#### Buffer Sizes

```c
// Path and filename buffers
MAX_PATH_LENGTH                  // 4096 - For full file paths
MAX_MEDIUM_PATH_LENGTH           // 2048 - For medium-length paths
MAX_TEMP_PATH_LENGTH             // 1024 - For temporary path buffers

// Package and version strings
MAX_PACKAGE_NAME_LENGTH          // 256  - For "author/name" strings
MAX_VERSION_STRING_LENGTH        // 32   - For "X.Y.Z" version strings
MAX_VERSION_STRING_MEDIUM_LENGTH // 64   - For longer version strings

// General purpose buffers
MAX_ERROR_MESSAGE_LENGTH         // 4096 - For error messages
MAX_TEMP_BUFFER_LENGTH           // 512  - For temporary text buffers
MAX_RANGE_STRING_LENGTH          // 128  - For version range strings
MAX_TERM_STRING_LENGTH           // 256  - For term strings
MAX_KEY_LENGTH                   // 256  - For key strings (JSON, etc.)
MAX_USER_AGENT_LENGTH            // 64   - For User-Agent strings
```

#### Initial Capacities for Dynamic Arrays

```c
INITIAL_SMALL_CAPACITY           // 16  - Small collections
INITIAL_MEDIUM_CAPACITY          // 64  - Medium collections
INITIAL_LARGE_CAPACITY           // 256 - Large collections
INITIAL_REGISTRY_CAPACITY        // 128 - Registry entries

// Specific types (use when appropriate)
INITIAL_MODULE_CAPACITY          // 16
INITIAL_PACKAGE_CAPACITY         // 64
INITIAL_FILE_CAPACITY            // 256
INITIAL_VISITED_CAPACITY         // 64
INITIAL_CONNECTION_CAPACITY      // 32
INITIAL_PLAN_CAPACITY            // 32
```

#### File and Directory Permissions

```c
DIR_PERMISSIONS                  // 0755 - Standard directory permissions
```

#### Unit Conversion Constants

```c
BYTES_PER_KB                     // 1024.0
BYTES_PER_MB                     // 1024 * 1024
MICROSECONDS_PER_SECOND          // 1000000.0
PROGRESS_BYTES_PER_DOT           // 50 * 1024
```

#### Memory Allocation

```c
INITIAL_ARENA_SIZE               // BYTES_PER_MB
```

### Exit Codes (`src/exit_codes.h`)

For command return codes:

```c
#include "exit_codes.h"          // Or ../../exit_codes.h, etc.

EXIT_SUCCESS                     // 0
EXIT_GENERAL_ERROR               // 1
EXIT_NO_UPGRADES_AVAILABLE       // 100
```

### HTTP Status Codes (`src/http_constants.h`)

For HTTP response handling:

```c
#include "http_constants.h"

// Range constants
HTTP_SUCCESS_MIN                 // 200
HTTP_SUCCESS_MAX                 // 299
HTTP_CLIENT_ERROR_MIN            // 400
HTTP_CLIENT_ERROR_MAX            // 499
HTTP_SERVER_ERROR_MIN            // 500

// Helper functions (preferred)
http_is_success(long code)       // Returns 1 if 200-299
http_is_client_error(long code)  // Returns 1 if 400-499
http_is_server_error(long code)  // Returns 1 if >= 500
```

### Terminal Colors (`src/terminal_colors.h`)

For colored terminal output:

```c
#include "terminal_colors.h"

ANSI_GREEN                       // "\033[32m"
ANSI_BRIGHT_GREEN                // "\033[1;32m"
ANSI_RED                         // "\033[31m"
ANSI_YELLOW                      // "\033[33m"
ANSI_CYAN                        // "\033[36m"
ANSI_DULL_CYAN                   // "\033[36m"
ANSI_DULL_YELLOW                 // "\033[33m"
ANSI_RESET                       // "\033[0m"
```

### Examples

#### ✅ Correct Usage

```c
#include "alloc.h"
#include "constants.h"

// Buffer declarations
char filepath[MAX_PATH_LENGTH];
char package_name[MAX_PACKAGE_NAME_LENGTH];
char version[MAX_VERSION_STRING_LENGTH];
char error_msg[MAX_ERROR_MESSAGE_LENGTH];

// Dynamic array initialization
int modules_capacity = INITIAL_MODULE_CAPACITY;
char **modules = arena_malloc(modules_capacity * sizeof(char*));

// Directory creation
mkdir(path, DIR_PERMISSIONS);

// HTTP status checking
if (http_is_success(response_code)) {
    printf("Downloaded index.dat (%.1f KB)\n",
           (double)dl_size / BYTES_PER_KB);
}

// Exit codes
return EXIT_NO_UPGRADES_AVAILABLE;

// Terminal colors
printf("%sSuccess!%s\n", ANSI_GREEN, ANSI_RESET);
```

#### ❌ Incorrect Usage (Magic Numbers)

```c
// NEVER DO THIS!
char filepath[4096];                    // Magic number
char package_name[256];                 // Magic number
char version[32];                       // Magic number

int modules_capacity = 16;              // Magic number
modules = arena_malloc(16 * sizeof(char*));  // Magic number

mkdir(path, 0755);                      // Magic number

if (response_code >= 200 && response_code < 300) {  // Magic numbers
    printf("Downloaded (%.1f KB)\n",
           (double)dl_size / 1024.0);   // Magic number
}

return 100;                             // Magic number

printf("\033[32mSuccess!\033[0m\n");    // Magic strings
```

### When to Add New Constants

If you need a buffer size or constant that doesn't exist:

1. **Check if an existing constant fits** - Don't create duplicates
2. **Add it to `src/constants.h`** with a clear comment
3. **Use a descriptive name** following the existing pattern
4. **Document it in this file** under the appropriate section

Example addition to `constants.h`:

```c
/*
 * New category of constants
 */
#define MAX_COMMIT_MESSAGE_LENGTH 500   // For git commit messages
#define INITIAL_COMMIT_CAPACITY 32      // Initial commits array size
```

### Verification Before Committing

Before requesting a commit, verify no new magic numbers were introduced:

```bash
# Check for common magic number patterns (will have false positives)
grep -n "char.*\[256\]" src/your_file.c
grep -n "char.*\[1024\]" src/your_file.c
grep -n "capacity = 16" src/your_file.c
grep -n "mkdir.*0755" src/your_file.c
```

### Tree-Sitter Field Names

When working with tree-sitter AST nodes, use predefined field name constants:

```c
#include "constants.h"

// Instead of:
ts_node_child_by_field_name(node, "operator", 8);  // WRONG

// Use:
ts_node_child_by_field_name(node, FIELD_OPERATOR, FIELD_OPERATOR_LEN);
```

Available fields:
- `FIELD_OPERATOR` / `FIELD_OPERATOR_LEN`
- `FIELD_ASSOCIATIVITY` / `FIELD_ASSOCIATIVITY_LEN`
- `FIELD_PRECEDENCE` / `FIELD_PRECEDENCE_LEN`

---

## Summary

**Golden Rule #1**: In application code, always use `arena_*` functions. Never use standard allocation functions directly.

**Golden Rule #2**: No magic numbers for buffer sizes. Always use constants from `src/constants.h`.

**Golden Rule #3**: Track capacity in a variable and check before writing to dynamic arrays.

**Golden Rule #4**: Use helper functions like `http_is_success()` instead of hard-coded range checks.

**No commits**: NEVER commit to Git yourself.

**Building the project**: `make rebuild`

**NEVER DELETE ANYTHING**: Absolutely NEVER EVER run `rm` command or delete anything with git file removal commands.