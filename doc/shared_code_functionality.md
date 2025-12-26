# Shared Code Functionality

This document describes the shared modules and functions available for use when implementing new commands or extending existing functionality in **elm-wrap**. Using these shared modules ensures consistency and reduces code duplication. Note that the project is called **elm-wrap**, but the binary is named `wrap`.

## Overview

The codebase provides several shared modules organized by functionality:

| Module | Header | Purpose |
|--------|--------|---------|
| Command Common | `elm_cmd_common.h` | Elm compiler invocation utilities |
| Elm Project | `elm_project.h` | Elm project file parsing and traversal |
| File Utilities | `fileutil.h` | General file system operations |
| Local Dev Tracking | `local_dev/local_dev_tracking.h` | Query local development tracking relationships |
| Package List | `shared/package_list.h` | Consistent package list printing and sorting |
| Rulr Host Helpers | `rulr/host_helpers.h` | Inserting facts into Rulr engine |

---

## elm_cmd_common.h — Elm Command Utilities

**Include:** `#include "elm_cmd_common.h"`

This module provides shared functionality for commands that wrap the Elm compiler (make, reactor, bump, repl, publish, diff).

### Functions

#### `char **build_elm_environment(void)`

Builds an environment array suitable for `execve()` when running elm commands.

By default, this adds `https_proxy=http://1` to force elm into offline mode, since `wrap` pre-downloads all packages. Set the environment variable `WRAP_ALLOW_ELM_ONLINE=1` to skip this and allow elm to access the network.

Note: In the current implementation, `WRAP_ALLOW_ELM_ONLINE` is treated as a boolean flag (any non-empty value enables online mode).

**Returns:** Arena-allocated environment array, or NULL on failure.

**Example:**
```c
char **elm_env = build_elm_environment();
if (!elm_env) {
    log_error("Failed to build environment");
    return 1;
}
execve(elm_path, elm_args, elm_env);
```

#### `int download_all_packages(ElmJson *elm_json, InstallEnv *env)`

Downloads all packages listed in elm.json before invoking the elm compiler.

For **application projects**, downloads:
- Direct dependencies
- Indirect dependencies  
- Test direct dependencies
- Test indirect dependencies

For **package projects**, resolves version constraints and downloads:
- Package dependencies
- Test dependencies

**Returns:** 0 on success, non-zero on failure.

**Example:**
```c
ElmJson *elm_json = elm_json_read("elm.json");
InstallEnv *env = install_env_create();
install_env_init(env);

int result = download_all_packages(elm_json, env);
if (result != 0) {
    log_error("Failed to download dependencies");
    return 1;
}
```

---

## elm_project.h — Elm Project Utilities

**Include:** `#include "elm_project.h"`

This module provides utilities for parsing elm.json and working with Elm source files.

### Functions

#### `char **elm_parse_exposed_modules(const char *elm_json_path, int *count)`

Parses the `exposed-modules` field from elm.json.

Handles both formats:
- Flat array: `["Module1", "Module2"]`
- Categorized object: `{ "Category": ["Module1", "Module2"], ... }`

**Returns:** Arena-allocated array of module names, or NULL on error.

**Example:**
```c
int exposed_count = 0;
char **exposed = elm_parse_exposed_modules("elm.json", &exposed_count);
for (int i = 0; i < exposed_count; i++) {
    printf("Exposed: %s\n", exposed[i]);
}
```

#### `char **elm_parse_source_directories(const char *elm_json_path, int *count)`

Parses the `source-directories` field from elm.json.

**Returns:** Arena-allocated array of directory paths, or NULL on error.

#### `char *elm_module_name_to_path(const char *module_name, const char *src_dir)`

Converts an Elm module name to a file path.

**Example:**
```c
char *path = elm_module_name_to_path("Html.Events", "src");
// path = "src/Html/Events.elm"
```

#### `void elm_collect_elm_files(const char *dir_path, char ***files, int *count, int *capacity)`

Recursively collects all `.elm` files in a directory.

**Example:**
```c
int capacity = 64;
int count = 0;
char **files = arena_malloc(capacity * sizeof(char*));

elm_collect_elm_files("src", &files, &count, &capacity);
for (int i = 0; i < count; i++) {
    printf("Found: %s\n", files[i]);
}
```

#### `void elm_collect_all_files(const char *dir_path, char ***files, int *count, int *capacity)`

Recursively collects all files (not just .elm) in a directory. Useful for package publishing.

#### `int elm_is_file_in_list(const char *file, char **list, int count)`

Checks if a file path is in a list.

**Returns:** 1 if found, 0 otherwise.

---

## fileutil.h — File Utilities

**Include:** `#include "fileutil.h"`

General-purpose file system utilities.

### Functions

#### `bool file_exists(const char *path)`

Checks if a regular file exists at the given path.

**Returns:** `true` if file exists and is a regular file, `false` otherwise.

**Example:**
```c
if (!file_exists("elm.json")) {
    log_error("elm.json not found");
    return 1;
}
```

#### `char *file_read_contents_bounded(const char *filepath, size_t max_bytes, size_t *out_size)`

Reads entire file contents into an arena-allocated buffer, enforcing an upper bound on file size.

**Returns:** Null-terminated string, or NULL on failure (missing file, too large, read error, allocation failure).

**Example:**
```c
char *content = file_read_contents_bounded("elm.json", MAX_ELM_JSON_FILE_BYTES, NULL);
if (!content) {
    log_error("Failed to read file (missing/too-large/read error)");
    return 1;
}
cJSON *json = cJSON_Parse(content);
```

#### `char *file_read_contents(const char *filepath)`

Legacy convenience wrapper.

Prefer `file_read_contents_bounded()` so callers can pick an appropriate limit for the file type.

#### `char *strip_trailing_slash(const char *path)`

Returns an arena-allocated copy of the path with trailing slashes removed.

**Example:**
```c
char *clean = strip_trailing_slash("/path/to/dir/");
// clean = "/path/to/dir"
```

### Additional File Utilities

| Function | Description |
|----------|-------------|
| `extract_zip()` | Extract a ZIP file to a destination directory |
| `extract_zip_selective()` | Extract only elm.json, docs.json, LICENSE, README.md, src/ |
| `find_first_subdirectory()` | Find the first subdirectory in a directory |
| `move_directory_contents()` | Move contents, flattening by one level |
| `remove_directory_recursive()` | Recursively delete a directory |
| `copy_directory_recursive()` | Recursively copy a directory |
| `copy_directory_selective()` | Copy only package-relevant files |

---

## local_dev/local_dev_tracking.h — Local Development Tracking

**Include:** `#include "local_dev/local_dev_tracking.h"`

This module provides a shared C API in `src/local_dev/` for querying local development package tracking relationships.

### Overview

The local-dev tracking system maintains bidirectional relationships between:
- **Applications** that use local-dev packages
- **Packages** being developed locally

The tracking data is stored in the filesystem under `WRAP_HOME/_local-dev/packages/`.

In the current implementation, `get_local_dev_tracking_dir()` resolves to `WRAP_HOME/_local-dev`, and all tracking files live under that directory.

### Directory Structure

```
WRAP_HOME/_local-dev/
└── author/
    └── name/
        └── version/
            ├── <hash1>    # Contains: /path/to/app1/elm.json
            └── <hash2>    # Contains: /path/to/app2/elm.json
```

Each hash file contains the absolute path to an application's `elm.json` that is tracking the package.

### Types

#### `LocalDevPackage`

```c
typedef struct {
    char *author;   // Package author (arena-allocated)
    char *name;     // Package name (arena-allocated)
    char *version;  // Package version (arena-allocated)
} LocalDevPackage;
```

Represents a local-dev package with its full identity.

### Functions

#### `LocalDevPackage *local_dev_get_tracked_packages(const char *elm_json_path, int *out_count)`

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

#### `char **local_dev_get_tracking_apps(const char *author, const char *name, const char *version, int *out_count)`

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

#### `void local_dev_package_free(LocalDevPackage *pkg)`

Free the contents of a single `LocalDevPackage` struct.

**Parameters:**
- `pkg` - Pointer to the package (does not free the struct itself)

**Note:** This frees `author`, `name`, and `version` strings but not the struct pointer.

#### `void local_dev_packages_free(LocalDevPackage *pkgs, int count)`

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

#### `bool is_package_local_dev(const char *author, const char *name, const char *version)`

Check if a specific package version is registered for local development.

In the current implementation, this returns true when the tracking directory exists at:
`get_local_dev_tracking_dir()/author/name/version`.

### Usage Patterns

#### Finding packages tracked by an application

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

#### Finding applications tracking a package

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

#### Displaying tracking info

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

### Dependencies

This module depends on:
- `install_local_dev.h` - For `get_local_dev_tracking_dir()`
- `alloc.h` - For arena allocation functions
- `constants.h` - For `MAX_PATH_LENGTH`, `INITIAL_SMALL_CAPACITY`
- `fileutil.h` - For `file_read_contents_bounded()`

### Files

| File | Description |
|------|-------------|
| `src/local_dev/local_dev_tracking.h` | Public API header |
| `src/local_dev/local_dev_tracking.c` | Implementation |

### Related Documentation

- [Installing a package for local development](package_install_local_dev.md) - User-facing workflow

---

## shared/package_list.h — Package List Printing and Sorting

**Include:** `#include "shared/package_list.h"`

This module provides consistent formatting and sorting for package lists across all commands. All packages are sorted by author/name alphabetically.

### When to Use

- Simple package lists without version change arrows (use print functions)
- Any place needing sorted package names (use comparison functions)

For complex formatting (upgrade arrows, color coding, constraint display), you may need custom printing, but use the comparison functions for sorting.

### Types

#### `PackageListEntry`

```c
typedef struct {
    const char *author;       /* Package author (e.g., "elm") */
    const char *name;         /* Package name (e.g., "core") */
    const char *version;      /* Version string or NULL */
    const char *annotation;   /* Optional annotation (e.g., " (indirect)") */
} PackageListEntry;
```

### Functions

#### `int package_list_compare(const void *a, const void *b)`

Comparison function for sorting PackageListEntry arrays. Sorts by author first, then by name.

**Example:**
```c
qsort(entries, count, sizeof(PackageListEntry), package_list_compare);
```

#### `int package_name_compare(const void *a, const void *b)`

Comparison function for sorting strings in "author/name" format.

**Example:**
```c
qsort(names, count, sizeof(char*), package_name_compare);
```

#### `void package_list_print_sorted(entries, count, max_width, indent)`

Print a sorted package list with aligned versions. Creates a sorted copy internally.

**Parameters:**
- `entries` - Array of PackageListEntry
- `count` - Number of entries
- `max_width` - Maximum name width for alignment (0 = auto-calculate)
- `indent` - Number of leading spaces

**Example:**
```c
PackageListEntry entries[3] = {
    { "elm", "core", "1.0.5", NULL },
    { "avh4", "elm-color", "1.0.0", NULL },
    { "elm", "html", "1.0.0", " (indirect)" }
};
package_list_print_sorted(entries, 3, 0, 2);
/* Output:
     avh4/elm-color  1.0.0
     elm/core        1.0.5
     elm/html        1.0.0 (indirect)
*/
```

### Files

| File | Description |
|------|-------------|
| `src/shared/package_list.h` | Public API header |
| `src/shared/package_list.c` | Implementation |

---

## rulr/host_helpers.h — Rulr Fact Insertion

**Include:** `#include "rulr/host_helpers.h"`

This module provides convenience functions for inserting facts into a Rulr engine from host code. Use these when extracting information from external sources (files, AST, etc.) and injecting it as facts for rule evaluation.

### Functions

#### `int rulr_insert_fact_1s(Rulr *r, const char *pred, const char *s1)`

Insert a fact with a single symbol argument.

If the predicate doesn't exist, it will be registered automatically.

**Returns:** Fact ID on success, -1 on failure.

**Example:**
```c
rulr_insert_fact_1s(&rulr, "module", "Main");
rulr_insert_fact_1s(&rulr, "exported_value", "view");
```

#### `int rulr_insert_fact_2s(Rulr *r, const char *pred, const char *s1, const char *s2)`

Insert a fact with two symbol arguments.

**Example:**
```c
rulr_insert_fact_2s(&rulr, "import_alias", "Html", "H");
rulr_insert_fact_2s(&rulr, "type_annotation", "update", "Msg -> Model -> Model");
```

#### `int rulr_insert_fact_3s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3)`

Insert a fact with three symbol arguments.

**Example:**
```c
rulr_insert_fact_3s(&rulr, "dependency", "elm", "core", "1.0.5");
rulr_insert_fact_3s(&rulr, "package_file_info", abs_path, rel_path, filename);
```

#### `int rulr_insert_fact_4s(Rulr *r, const char *pred, const char *s1, const char *s2, const char *s3, const char *s4)`

Insert a fact with four symbol arguments.

If the predicate doesn't exist, it will be registered automatically.

**Returns:** Fact ID on success, -1 on failure.

### Common Fact Patterns

When writing Rulr rules, these are commonly inserted host facts:

| Predicate | Arity | Description |
|-----------|-------|-------------|
| `module(name)` | 1 | Module name |
| `file_path(path)` | 1 | Source file path |
| `import(module)` | 1 | Imported module |
| `import_alias(module, alias)` | 2 | Import alias |
| `import_exposing(module, name)` | 2 | Exposed import |
| `exported_value(name)` | 1 | Exported function/value |
| `exported_type(name)` | 1 | Exported type |
| `type_annotation(name, type)` | 2 | Type annotation |
| `type_alias(name)` | 1 | Type alias declaration |
| `union_type(name)` | 1 | Union type declaration |
| `constructor(type, name)` | 2 | Union type constructor |
| `dependency(author, package, version)` | 3 | Package dependency |
| `exposed_module(name)` | 1 | Package exposed module |
| `source_file(path)` | 1 | Source file in package |
| `file_module(file, module)` | 2 | File to module mapping |
| `file_import(file, module)` | 2 | Import in a specific file |

---

## Program Name Access

**Include:** `#include "global_context.h"`

To display the actual program name in error messages and usage text, use the global context function instead of hard-coding "wrap":

### `const char *global_context_program_name(void)`

Returns the actual program name (from `argv[0]`).

This allows the binary to work correctly even if renamed or aliased by the user.

**Returns:** The program name as invoked, or the value from `buildinfo.h` (`build_program_name`) if not initialized. The `build_program_name` constant is automatically synchronized with the Makefile's `TARGET_FILE` variable during build.

**Example:**
```c
static void print_usage(void) {
    printf("Usage: %s command [OPTIONS]\n", global_context_program_name());
    printf("Run '%s --help' for more information\n", global_context_program_name());
}

// In error messages
fprintf(stderr, "Error: Failed to initialize\n");
fprintf(stderr, "Hint: Run '%s repository new' to create a repository\n",
        global_context_program_name());
```

**Important:**
- The global context is initialized early in `main()` with `argv[0]`
- The program name is extracted internally using `basename(argv[0])`
- Never hard-code "wrap" in user-facing messages
- Subcommands can safely call `global_context_init(argv[0])` again; it will return the existing context

---

## Memory Management

All shared modules follow the arena allocator pattern. See `AGENTS.md` for details.

**Key points:**
- Use `arena_malloc()`, `arena_strdup()`, etc. — never raw `malloc()`
- Returned strings/arrays are arena-allocated
- No need to free individual allocations; arena cleanup handles it

---

## Adding New Commands

When implementing a new command that wraps the Elm compiler:

1. Include `elm_cmd_common.h` for `build_elm_environment()` and `download_all_packages()`
2. Include `elm_json.h` for parsing elm.json
3. Include `install_env.h` for package download environment

When implementing rules-based analysis:

1. Include `rulr/host_helpers.h` for fact insertion
2. Include `elm_project.h` for file collection
3. Include `ast/skeleton.h` for lightweight AST parsing
4. Follow the pattern in `review.c` or `package_publish.c`

---

## See Also

- `AGENTS.md` — Memory allocation rules
- `doc/rulr_compiled_format.md` — Rulr compiled rule format
- `src/ast/skeleton.h` — Lightweight Elm AST parsing
