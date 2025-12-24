# Local Development Tracking - Shared Code API

This document describes the shared C API in `src/local_dev/` for querying local development package tracking relationships.

## Overview

The local-dev tracking system maintains bidirectional relationships between:
- **Applications** that use local-dev packages
- **Packages** being developed locally

This module provides query functions to traverse these relationships. The tracking data is stored in the filesystem under `WRAP_HOME/_local-dev/packages/`.

## Directory Structure

```
WRAP_HOME/_local-dev/packages/
└── author/
    └── name/
        └── version/
            ├── <hash1>    # Contains: /path/to/app1/elm.json
            └── <hash2>    # Contains: /path/to/app2/elm.json
```

Each hash file contains the absolute path to an application's `elm.json` that is tracking the package.

## Header File

```c
#include "local_dev/local_dev_tracking.h"
```

## Types

### LocalDevPackage

```c
typedef struct {
    char *author;   // Package author (arena-allocated)
    char *name;     // Package name (arena-allocated)
    char *version;  // Package version (arena-allocated)
} LocalDevPackage;
```

Represents a local-dev package with its full identity.

## Functions

### local_dev_get_tracked_packages

```c
LocalDevPackage *local_dev_get_tracked_packages(const char *elm_json_path, int *out_count);
```

Get all local-dev packages that an application is tracking.

**Parameters:**
- `elm_json_path` - Path to the application's `elm.json` file
- `out_count` - Output parameter: number of packages found

**Returns:**
- Arena-allocated array of `LocalDevPackage` structs
- `NULL` if no packages are being tracked or on error
- Caller must free with `local_dev_packages_free()`

**Example:**
```c
int count = 0;
LocalDevPackage *packages = local_dev_get_tracked_packages("elm.json", &count);

if (packages) {
    for (int i = 0; i < count; i++) {
        printf("Tracking: %s/%s %s\n",
               packages[i].author,
               packages[i].name,
               packages[i].version);
    }
    local_dev_packages_free(packages, count);
}
```

### local_dev_get_tracking_apps

```c
char **local_dev_get_tracking_apps(const char *author, const char *name,
                                   const char *version, int *out_count);
```

Get all applications that are tracking a specific local-dev package.

**Parameters:**
- `author` - Package author
- `name` - Package name
- `version` - Package version
- `out_count` - Output parameter: number of applications found

**Returns:**
- Arena-allocated array of absolute paths to `elm.json` files
- `NULL` if no applications are tracking or on error
- Caller must free each path and the array with `arena_free()`

**Example:**
```c
int count = 0;
char **app_paths = local_dev_get_tracking_apps("author", "name", "1.0.0", &count);

if (app_paths) {
    for (int i = 0; i < count; i++) {
        printf("Tracked by: %s\n", app_paths[i]);
        arena_free(app_paths[i]);
    }
    arena_free(app_paths);
}
```

### local_dev_package_free

```c
void local_dev_package_free(LocalDevPackage *pkg);
```

Free the contents of a single `LocalDevPackage` struct.

**Parameters:**
- `pkg` - Pointer to the package (does not free the struct itself)

**Note:** This frees `author`, `name`, and `version` strings but not the struct pointer.

### local_dev_packages_free

```c
void local_dev_packages_free(LocalDevPackage *pkgs, int count);
```

Free an array of `LocalDevPackage` structs and all their contents.

**Parameters:**
- `pkgs` - Array of packages to free
- `count` - Number of packages in the array

**Example:**
```c
LocalDevPackage *packages = local_dev_get_tracked_packages(path, &count);
// ... use packages ...
local_dev_packages_free(packages, count);
```

## Usage Patterns

### Finding packages tracked by an application

Used by `builder.c` to clean build artifacts before compilation:

```c
int pkg_count = 0;
LocalDevPackage *packages = local_dev_get_tracked_packages(elm_json_path, &pkg_count);

if (pkg_count > 0 && packages) {
    for (int i = 0; i < pkg_count; i++) {
        char *pkg_path = cache_get_package_path(cache,
            packages[i].author, packages[i].name, packages[i].version);

        if (pkg_path) {
            delete_artifacts_in_dir(pkg_path);
            arena_free(pkg_path);
        }
    }
    local_dev_packages_free(packages, pkg_count);
}
```

### Finding applications tracking a package

Used by `install_local_dev.c` to refresh dependent applications:

```c
int dep_count = 0;
char **dep_paths = local_dev_get_tracking_apps(author, name, version, &dep_count);

if (dep_count > 0 && dep_paths) {
    for (int i = 0; i < dep_count; i++) {
        refresh_app_indirect_deps(dep_paths[i], env, author, name, "elm.json");
        arena_free(dep_paths[i]);
    }
    arena_free(dep_paths);
}
```

### Displaying tracking info

Used by `info_cmd.c` to show tracking relationships:

```c
// For applications - show which packages they track
LocalDevPackage *packages = local_dev_get_tracked_packages(elm_json_path, &count);
if (packages) {
    printf("Tracking local dev packages:\n");
    for (int i = 0; i < count; i++) {
        printf("  %s/%s %s\n", packages[i].author, packages[i].name, packages[i].version);
    }
    local_dev_packages_free(packages, count);
}

// For packages - show which applications track them
char **apps = local_dev_get_tracking_apps(author, name, version, &count);
if (apps) {
    printf("Tracked by applications:\n");
    for (int i = 0; i < count; i++) {
        printf("  %s\n", apps[i]);
        arena_free(apps[i]);
    }
    arena_free(apps);
}
```

## Dependencies

This module depends on:
- `install_local_dev.h` - For `get_local_dev_tracking_dir()`
- `alloc.h` - For arena allocation functions
- `constants.h` - For `MAX_PATH_LENGTH`, `INITIAL_SMALL_CAPACITY`
- `fileutil.h` - For `file_read_contents_bounded()`

## Files

| File | Description |
|------|-------------|
| `src/local_dev/local_dev_tracking.h` | Public API header |
| `src/local_dev/local_dev_tracking.c` | Implementation |

## Related Documentation

- [Installing a package for local development](package_install_local_dev.md) - User-facing workflow
- [Shared Code Functionality](shared_code_functionality.md) - Other shared modules
