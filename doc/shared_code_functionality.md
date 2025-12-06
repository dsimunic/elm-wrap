# Shared Code Functionality

This document describes the shared modules and functions available for use when implementing new commands or extending existing functionality in **elm-wrap**. Using these shared modules ensures consistency and reduces code duplication. Note that the project is called **elm-wrap**, but the binary is named `wrap`.

## Overview

The codebase provides several shared modules organized by functionality:

| Module | Header | Purpose |
|--------|--------|---------|
| Command Common | `elm_cmd_common.h` | Elm compiler invocation utilities |
| Elm Project | `elm_project.h` | Elm project file parsing and traversal |
| File Utilities | `fileutil.h` | General file system operations |
| Rulr Host Helpers | `rulr/host_helpers.h` | Inserting facts into Rulr engine |

---

## elm_cmd_common.h — Elm Command Utilities

**Include:** `#include "elm_cmd_common.h"`

This module provides shared functionality for commands that wrap the Elm compiler (make, reactor, bump, repl, publish, diff).

### Functions

#### `char **build_elm_environment(void)`

Builds an environment array suitable for `execve()` when running elm commands.

By default, this adds `https_proxy=http://1` to force elm into offline mode, since `wrap` pre-downloads all packages. Set the environment variable `WRAP_ALLOW_ELM_ONLINE=1` to skip this and allow elm to access the network.

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

#### `char *file_read_contents(const char *filepath)`

Reads entire file contents into an arena-allocated buffer.

**Returns:** Null-terminated string, or NULL on failure.

**Example:**
```c
char *content = file_read_contents("elm.json");
if (!content) {
    log_error("Failed to read file");
    return 1;
}
cJSON *json = cJSON_Parse(content);
```

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
