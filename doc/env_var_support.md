# Environment Variable Support in C Applications

This document describes a robust mechanism for handling environment variables in C applications, providing compiled-in default values that can be overridden at runtime. This approach ensures applications work out-of-the-box with sensible defaults while remaining configurable through environment variables.

## Overview

The system consists of three main components:

1. **Environment Defaults File** - A text file containing key=value pairs with default values
2. **Build-time Processing** - Makefile rules that parse the defaults file and embed them into the binary
3. **Runtime API** - C functions that check environment variables first, then fall back to compiled defaults

## Environment Defaults File Format

Create a file named `ENV_DEFAULTS` (or similar) with the following format:

```
# Comments are not supported - each line is KEY=VALUE
MY_APP_DATA_DIR=~/.myapp
MY_APP_CACHE_DIR=/tmp/myapp-cache
MY_APP_API_URL=https://api.example.com
MY_APP_DEBUG_LEVEL=info
```

**Rules:**
- One `KEY=VALUE` pair per line
- No spaces around the `=` sign
- Values can contain any characters except newlines
- Empty values are allowed
- Lines not matching `KEY=VALUE` format are ignored

## Build-time Integration

### Makefile Configuration

Add the following to your `Makefile`:

```makefile
# Configuration
ENV_DEFAULTS_FILE = ENV_DEFAULTS

# Default values if file doesn't exist
ENV_DEFAULT_MY_APP_DATA_DIR ?= ~/.myapp
ENV_DEFAULT_MY_APP_CACHE_DIR ?= /tmp/myapp-cache
ENV_DEFAULT_MY_APP_API_URL ?= https://api.example.com
ENV_DEFAULT_MY_APP_DEBUG_LEVEL ?= info

# Parse ENV_DEFAULTS file if it exists
ifneq ($(wildcard $(ENV_DEFAULTS_FILE)),)
  ENV_DEFAULT_MY_APP_DATA_DIR := $(shell grep '^MY_APP_DATA_DIR=' $(ENV_DEFAULTS_FILE) 2>/dev/null | cut -d= -f2-)
  ENV_DEFAULT_MY_APP_CACHE_DIR := $(shell grep '^MY_APP_CACHE_DIR=' $(ENV_DEFAULTS_FILE) 2>/dev/null | cut -d= -f2-)
  ENV_DEFAULT_MY_APP_API_URL := $(shell grep '^MY_APP_API_URL=' $(ENV_DEFAULTS_FILE) 2>/dev/null | cut -d= -f2-)
  ENV_DEFAULT_MY_APP_DEBUG_LEVEL := $(shell grep '^MY_APP_DEBUG_LEVEL=' $(ENV_DEFAULTS_FILE) 2>/dev/null | cut -d= -f2-)
endif
```

### Build Info Generation

If you have a build info system (like `buildinfo.mk`), extend it to include environment defaults:

```makefile
# In your buildinfo generation target
generate-buildinfo:
    # ... existing buildinfo generation ...
    @echo "/* Environment variable defaults */" >> $(BUILDDIR)/buildinfo.c
    @echo "const char *env_default_my_app_data_dir = \"$(ENV_DEFAULT_MY_APP_DATA_DIR)\";" >> $(BUILDDIR)/buildinfo.c
    @echo "const char *env_default_my_app_cache_dir = \"$(ENV_DEFAULT_MY_APP_CACHE_DIR)\";" >> $(BUILDDIR)/buildinfo.c
    @echo "const char *env_default_my_app_api_url = \"$(ENV_DEFAULT_MY_APP_API_URL)\";" >> $(BUILDDIR)/buildinfo.c
    @echo "const char *env_default_my_app_debug_level = \"$(ENV_DEFAULT_MY_APP_DEBUG_LEVEL)\";" >> $(BUILDDIR)/buildinfo.c
```

### Header File Declaration

Create `env_defaults.h`:

```c
#ifndef ENV_DEFAULTS_H
#define ENV_DEFAULTS_H

#include <stdbool.h>

/**
 * Environment variable lookup with compiled-in defaults.
 * 
 * These functions check the environment variable first, and if not set,
 * return the default value from the ENV_DEFAULTS file (compiled into the binary).
 * 
 * Returns arena-allocated strings that expand ~ to the user's home directory.
 */

/* Get MY_APP_DATA_DIR with fallback to compiled default */
char *env_get_my_app_data_dir(void);

/* Get MY_APP_CACHE_DIR with fallback to compiled default */
char *env_get_my_app_cache_dir(void);

/* Get MY_APP_API_URL with fallback to compiled default */
char *env_get_my_app_api_url(void);

/* Get MY_APP_DEBUG_LEVEL with fallback to compiled default */
char *env_get_my_app_debug_level(void);

/* Check if debug mode is enabled (example boolean conversion) */
bool env_get_debug_mode(void);

#endif /* ENV_DEFAULTS_H */
```

## Runtime Implementation

### Core Implementation (`env_defaults.c`)

```c
#include "env_defaults.h"
#include "buildinfo.h"  // Contains the compiled defaults
#include "alloc.h"      // Your memory allocator
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Expand ~ to the user's home directory in a path */
static char *expand_tilde(const char *path) {
    if (!path || path[0] == '\0') {
        return arena_strdup("");
    }
    
    if (path[0] != '~') {
        return arena_strdup(path);
    }
    
    /* Handle ~/... or just ~ */
    if (path[1] == '\0' || path[1] == '/') {
        const char *home = getenv("HOME");
        if (!home) {
            /* If HOME is not set, return path as-is */
            return arena_strdup(path);
        }
        
        size_t home_len = strlen(home);
        size_t path_len = strlen(path);
        size_t result_len = home_len + path_len; /* -1 for ~ but +1 for nul */
        
        char *result = arena_malloc(result_len);
        if (!result) {
            return NULL;
        }
        
        strcpy(result, home);
        strcat(result, path + 1); /* Skip the ~ */
        return result;
    }
    
    /* ~user/... format is not supported, return as-is */
    return arena_strdup(path);
}

char *env_get_my_app_data_dir(void) {
    const char *env_val = getenv("MY_APP_DATA_DIR");
    if (env_val && env_val[0] != '\0') {
        return expand_tilde(env_val);
    }
    return expand_tilde(env_default_my_app_data_dir);
}

char *env_get_my_app_cache_dir(void) {
    const char *env_val = getenv("MY_APP_CACHE_DIR");
    if (env_val && env_val[0] != '\0') {
        return expand_tilde(env_val);
    }
    return expand_tilde(env_default_my_app_cache_dir);
}

char *env_get_my_app_api_url(void) {
    const char *env_val = getenv("MY_APP_API_URL");
    if (env_val && env_val[0] != '\0') {
        return arena_strdup(env_val);
    }
    return arena_strdup(env_default_my_app_api_url);
}

char *env_get_my_app_debug_level(void) {
    const char *env_val = getenv("MY_APP_DEBUG_LEVEL");
    if (env_val && env_val[0] != '\0') {
        return arena_strdup(env_val);
    }
    return arena_strdup(env_default_my_app_debug_level);
}

bool env_get_debug_mode(void) {
    const char *env_val = getenv("MY_APP_DEBUG");
    if (env_val && strcmp(env_val, "1") == 0) {
        return true;
    }
    /* Could also check debug level, but this is a separate boolean flag */
    return false;
}
```

### Build Info Header (`buildinfo.h`)

Add to your existing `buildinfo.h`:

```c
/* Environment variable defaults (from ENV_DEFAULTS file at build time) */
extern const char *env_default_my_app_data_dir;
extern const char *env_default_my_app_cache_dir;
extern const char *env_default_my_app_api_url;
extern const char *env_default_my_app_debug_level;
```

## Advanced Features

### Path Construction

For derived paths that combine multiple environment variables:

```c
char *env_get_my_app_config_file(void) {
    /* Get base config directory */
    char *config_dir = env_get_my_app_data_dir();
    if (!config_dir || config_dir[0] == '\0') {
        return NULL;
    }
    
    /* Get relative config file path from env or default */
    const char *rel_path = getenv("MY_APP_CONFIG_FILE");
    if (!rel_path || rel_path[0] == '\0') {
        rel_path = "config.ini";
    }
    
    /* Concatenate paths */
    size_t dir_len = strlen(config_dir);
    size_t rel_len = strlen(rel_path);
    size_t result_len = dir_len + 1 + rel_len + 1; /* +1 for / and +1 for nul */
    
    char *result = arena_malloc(result_len);
    if (!result) {
        arena_free(config_dir);
        return NULL;
    }
    
    snprintf(result, result_len, "%s/%s", config_dir, rel_path);
    arena_free(config_dir);
    return result;
}
```

### Type Conversion

For non-string values, add conversion functions:

```c
int env_get_my_app_timeout_seconds(void) {
    const char *env_val = getenv("MY_APP_TIMEOUT");
    if (env_val && env_val[0] != '\0') {
        char *endptr;
        long val = strtol(env_val, &endptr, 10);
        if (*endptr == '\0' && val >= 0 && val <= INT_MAX) {
            return (int)val;
        }
    }
    /* Default timeout */
    return 30;
}
```

## Usage Examples

### Basic Usage

```c
#include "env_defaults.h"

void initialize_app(void) {
    char *data_dir = env_get_my_app_data_dir();
    char *cache_dir = env_get_my_app_cache_dir();
    char *api_url = env_get_my_app_api_url();
    
    printf("Data directory: %s\n", data_dir);
    printf("Cache directory: %s\n", cache_dir);
    printf("API URL: %s\n", api_url);
    
    /* Use the values... */
    
    /* Memory is managed by arena allocator - no manual free needed */
}
```

### Conditional Logic

```c
void setup_logging(void) {
    char *level = env_get_my_app_debug_level();
    
    if (strcmp(level, "debug") == 0) {
        set_log_level(LOG_DEBUG);
    } else if (strcmp(level, "info") == 0) {
        set_log_level(LOG_INFO);
    } else if (strcmp(level, "warn") == 0) {
        set_log_level(LOG_WARN);
    } else {
        set_log_level(LOG_ERROR);
    }
}
```

## Memory Management

This system assumes you're using an arena allocator (like the one described in the memory management guidelines). All returned strings are allocated with `arena_malloc()` or `arena_strdup()` and should not be manually freed.

If you're not using an arena allocator, you'll need to modify the functions to return `strdup()` allocated strings that the caller is responsible for freeing.

## Distribution

When distributing your application:

1. Include the `ENV_DEFAULTS` file in your package
2. Users can modify this file to change defaults
3. Environment variables always take precedence over the file
4. The file is read at build time, not runtime

## Testing

Test both scenarios:

```bash
# Test with defaults
./myapp

# Test with environment overrides
MY_APP_DATA_DIR=/tmp/test MY_APP_DEBUG=1 ./myapp

# Test with modified ENV_DEFAULTS file
echo "MY_APP_API_URL=https://staging.example.com" >> ENV_DEFAULTS
make clean all
./myapp
```

## Benefits

1. **Zero-configuration startup** - Applications work immediately with sensible defaults
2. **Runtime configurability** - Environment variables can override any setting
3. **Build-time customization** - Packagers can modify defaults without changing code
4. **Type safety** - Compile-time checking of default values
5. **Memory efficiency** - Defaults are compiled into the binary, not read from disk at runtime
6. **Path expansion** - Automatic `~` expansion for user directories
7. **Flexible deployment** - Same binary works in different environments with different configs</content>
<parameter name="filePath">/Volumes/Devel/Projects/elm-wrap/doc/env_var_support.md