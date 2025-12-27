# Feature Flags

Feature flags in **elm-wrap** allow hiding development or experimental commands from regular users while allowing developer access via environment variables. Feature flags use a hybrid compile-time/runtime approach where Makefile sets defaults (0=hidden, 1=visible), and runtime environment variables can override these defaults.

## How Feature Flags Work

Each feature flag has:
- A compile-time default set in the `Makefile` (e.g., `FEATURE_CACHE ?= 0`)
- A runtime override via environment variable (e.g., `WRAP_FEATURE_CACHE=1`)
- Conditional display in help/usage output
- Conditional command routing with error messages when disabled

## Steps to Add a New Feature Flag

### 1. Add Feature Declaration (`src/features.h`)

Add a new function declaration for the feature:

```c
/* Check if the 'newfeature' command group is enabled */
bool feature_newfeature_enabled(void);
```

### 2. Implement Feature Logic (`src/features.c`)

Add the implementation following the existing pattern:

```c
bool feature_newfeature_enabled(void) {
    int env_value = check_env_flag("WRAP_FEATURE_NEWFEATURE");
    if (env_value >= 0) {
        return env_value == 1;
    }
    return FEATURE_NEWFEATURE_DEFAULT != 0;
}
```

Add the compile-time default fallback:

```c
#ifndef FEATURE_NEWFEATURE_DEFAULT
#define FEATURE_NEWFEATURE_DEFAULT 0
#endif
```

### 3. Update Makefile

Add the feature flag default:

```makefile
FEATURE_NEWFEATURE ?= 0
```

Add to CFLAGS:

```makefile
CFLAGS += -DFEATURE_NEWFEATURE_DEFAULT=$(FEATURE_NEWFEATURE)
```

### 4. Update Usage Display (`src/main.c`)

In `print_usage()` or appropriate usage function, conditionally show the command:

```c
if (feature_newfeature_enabled()) {
    printf("  newfeature SUBCOMMAND     Description of newfeature commands\n");
}
```

### 5. Update Command Routing (`src/main.c`)

In the main command router, add feature check before routing:

```c
if (strcmp(argv[1], "newfeature") == 0) {
    if (!feature_newfeature_enabled()) {
        fprintf(stderr, "Error: Command 'newfeature' is not available in this build.\n");
        return 1;
    }
    return cmd_newfeature(argc - 1, argv + 1);
}
```

For sub-commands within an existing command group, add the check in the sub-command handler (e.g., in `cmd_package()` for `wrap package newsubcmd`).

### 6. Environment Variable

Document the environment variable for runtime override:

- `WRAP_FEATURE_NEWFEATURE`: "1" to enable, "0" to disable

## Building with Feature Flags

**Development build (features enabled):**
```bash
make FEATURE_CODE=1 FEATURE_PUBLISH=1 FEATURE_REVIEW=1 FEATURE_POLICY=1 FEATURE_CACHE=1 clean all
```

**Release build (features hidden by default):**
```bash
make rebuild  # Uses ?= defaults (0 = hidden)
```

**Runtime override:**
```bash
WRAP_FEATURE_CACHE=1 wrap code cache elm/core 
```

## Inventory of Existing Feature Flags

### `publish` Feature Flag
- **Purpose**: Hides publishing-related commands
- **Commands Hidden**:
  - `wrap publish` (deprecated wrapper)
  - `wrap package publish`
- **Commands Always Visible**: `wrap package docs`
- **Environment Variable**: `WRAP_FEATURE_PUBLISH`
- **Default**: Hidden (0)
- **Implementation**: Command routing in `main.c` and sub-command routing in `cmd_package()`

### `review` Feature Flag
- **Purpose**: Hides the `wrap review` command group for running review rules
- **Commands Hidden**: `wrap review SUBCOMMAND`
- **Environment Variable**: `WRAP_FEATURE_REVIEW`
- **Default**: Hidden (0)
- **Implementation**: Full command group routing in `main.c`

### `policy` Feature Flag
- **Purpose**: Hides the `wrap policy` command group for viewing and managing rulr policy rules
- **Commands Hidden**: `wrap policy SUBCOMMAND`
- **Environment Variable**: `WRAP_FEATURE_POLICY`
- **Default**: Hidden (0)
- **Implementation**: Full command group routing in `main.c`

### `cache` Feature Flag
- **Purpose**: Hides the `wrap package cache` subcommand for downloading packages
- **Commands Hidden**: `wrap package cache`
- **Environment Variable**: `WRAP_FEATURE_CACHE`
- **Default**: Hidden (0)
- **Implementation**: Sub-command routing in `cmd_package()`

## Notes

- All feature flags default to hidden (0) in release builds
- Environment variables allow runtime enabling for development/testing
- Error messages inform users when commands are not available in their build
- The `docs` subcommand under `package` is intentionally always visible regardless of feature flags