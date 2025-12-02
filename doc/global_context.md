# Global Context and Protocol Mode Detection

This document describes the global context system in elm-wrap, particularly how the program determines whether to operate in V1 (legacy Elm) or V2 (elm-wrap repository) protocol mode.

## Overview

The global context (`GlobalContext`) is a singleton structure initialized at program startup that holds configuration state affecting all commands. Its primary purposes are:

1. **Protocol Mode Detection** - Determine whether to use V1 (legacy Elm) or V2 (elm-wrap repository) protocol for package management
2. **Compiler Type Detection** - Identify the compiler backend (elm, lamdera, wrapc) to adjust available commands

## Protocol Modes

### V1 Mode (Legacy)

V1 mode emulates the existing Elm package management system:
- Packages are fetched from `package.elm-lang.org`
- Uses the standard Elm registry protocol
- Works with existing Elm tooling and workflows
- **Default mode** when no V2 repository is detected

### V2 Mode (elm-wrap repositories)

V2 mode uses elm-wrap's new repository system:
- Packages are managed through local repositories created with `repository new`
- Uses the V2 registry index format
- Enables advanced features like multiple registry sources and custom package hosting
- **Active only** when a valid V2 repository is detected

## Detection Logic

The protocol mode is determined at startup in `global_context_init()`:

```
1. Get repository root path
   - Check ELM_WRAP_REPOSITORY_LOCAL_PATH environment variable
   - Fall back to compiled default (~/.elm-wrap/repository)

2. Determine compiler name
   - Extract basename from ELM_WRAP_ELM_COMPILER_PATH if set
   - Default to "elm"

3. Determine compiler version
   - Run the compiler with --version flag
   - Parse the output for version pattern (X.Y.Z)

4. Check for V2 repository
   - Build path: <root>/<compiler>/<version>/
   - Example: ~/.elm-wrap/repository/elm/0.19.1/
   - If this directory exists → V2 mode
   - If not → V1 mode
```

### Decision Flowchart

```
START
  │
  ▼
Get repository root path
  │
  ├─── Not configured ───────────────────────► V1 MODE
  │
  ▼
Get compiler name (from env or "elm")
  │
  ▼
Get compiler version (run compiler --version)
  │
  ├─── Could not determine version ──────────► V1 MODE
  │
  ▼
Check if <root>/<compiler>/<version>/ exists
  │
  ├─── Directory exists ─────────────────────► V2 MODE
  │
  └─── Directory does not exist ─────────────► V1 MODE
```

## GlobalContext Structure

```c
typedef enum {
    COMPILER_ELM,     /* Standard Elm compiler */
    COMPILER_LAMDERA, /* Lamdera compiler (extended command set) */
    COMPILER_WRAPC,   /* wrapc compiler (minimal command set, make only) */
    COMPILER_UNKNOWN  /* Unknown compiler (treated like Elm) */
} CompilerType;

typedef struct {
    ProtocolMode protocol_mode;    // PROTOCOL_V1 or PROTOCOL_V2
    char *compiler_name;           // e.g., "elm", "lamdera", "wrapc"
    char *compiler_version;        // e.g., "0.19.1"
    CompilerType compiler_type;    // Detected compiler type
    char *repository_path;         // Full path (V2 only)
} GlobalContext;
```

## API Functions

| Function | Description |
|----------|-------------|
| `global_context_init()` | Initialize the global context (called at startup) |
| `global_context_get()` | Get the current context instance |
| `global_context_is_v2()` | Quick check if V2 mode is active |
| `global_context_mode_string()` | Returns "V1" or "V2" for display |
| `global_context_compiler_type()` | Returns the detected `CompilerType` enum value |
| `global_context_is_elm()` | Returns true if compiler is elm or unknown |
| `global_context_is_lamdera()` | Returns true if compiler is lamdera |
| `global_context_is_wrapc()` | Returns true if compiler is wrapc |

## Usage Example

```c
#include "global_context.h"

// Context is already initialized in main()
GlobalContext *ctx = global_context_get();

if (global_context_is_v2()) {
    // Use V2 protocol for package operations
    printf("Using repository: %s\n", ctx->repository_path);
} else {
    // Use V1 (legacy Elm) protocol
    printf("Using standard Elm registry\n");
}
```

## Viewing Current Mode

Run `elm-wrap config` to see the current protocol mode:

```
$ elm-wrap config
Protocol mode: V2
Repository path: /Users/dev/.elm-wrap/repository/elm/0.19.1
ELM_HOME: /Users/dev/.elm
Elm compiler version: 0.19.1
Elm compiler path: /usr/local/bin/elm
```

## Creating a V2 Repository

To switch from V1 to V2 mode, create a repository:

```bash
elm-wrap repository new
```

This creates the repository directory structure and downloads the registry index. Subsequent runs of elm-wrap will detect this repository and operate in V2 mode.

## Supported Compilers

elm-wrap recognizes the following compiler backends, each with different command sets:

### elm (default)

Standard Elm compiler with commands:
- `repl`, `init`, `reactor`, `make`, `install`, `bump`, `diff`, `publish`

### lamdera

Lamdera compiler with extended command set:
- `live`, `login`, `check`, `deploy`, `init`, `install`, `make`, `repl`, `reset`, `update`, `annotate`, `eval`

### wrapc

Minimal compiler wrapper supporting only:
- `make`

### Compiler Detection

The compiler is detected from the `ELM_WRAP_ELM_COMPILER_PATH` environment variable basename, defaulting to "elm". The `--help` output adjusts automatically to show only the commands supported by the detected compiler.

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `ELM_WRAP_REPOSITORY_LOCAL_PATH` | Root path for V2 repositories |
| `ELM_WRAP_ELM_COMPILER_PATH` | Path to Elm compiler (affects compiler name detection) |

## Related Files

- `src/global_context.h` - Header with type definitions and API
- `src/global_context.c` - Implementation
- `src/main.c` - Context initialization at startup
- `src/config.c` - Displays protocol mode to users
