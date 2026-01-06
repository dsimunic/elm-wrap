# Build Driver Design Document

## Overview

The build driver implements a complete Elm compilation pipeline that:

1. Resolves all package dependencies (using existing PubGrub solver)
2. Computes package build order based on inter-package dependencies
3. Discovers local modules by crawling source directories
4. Parses imports to build the module dependency graph
5. Computes module build order via topological sort
6. Determines parallel compilation batches
7. Detects cached artifacts to enable incremental builds

The output is a JSON build plan that serves as the single source of truth for compilation, suitable for driving parallel builds or integration with external build systems.

---

## Command Interface

### Default: JSON Build Plan

```
wrap build [OPTIONS] PATH [PATH...]

Options:
  --json              Output build plan as JSON to stdout (default)
  --quiet, -q         Suppress progress messages to stderr
  --help, -h          Show help

Arguments:
  PATH                Entry point Elm files (e.g., src/Main.elm)
```

### Check Subcommand: Human-Readable Preview

The `check` subcommand displays a human-readable build plan summary and optionally
proceeds with compilation:

```
wrap build check [OPTIONS] PATH [PATH...]

Options:
  -y, --yes           Skip confirmation prompt and proceed with build
  -n, --no            Show plan only, do not prompt or build
  -q, --quiet         Suppress progress messages to stderr
  -h, --help          Show help

Arguments:
  PATH                Entry point Elm files (e.g., src/Main.elm)
```

Example output:

```
---- Build Plan --------------------------------------------------

Build plan for: `src/Main.elm`

  Include cached data for 36 already built packages.

  Rebuild 3 packages that are out of date:
      author/package1
      author/package2
      author/package3 (local-dev)

  Build 188 modules from the source paths:
     src/         : 180 modules
     ../forms/src :   8 modules

Do you want to proceed with build? [Y/n]
```

When the user confirms (or uses `-y`), the command runs `wrap make` with the
same entry point files to perform actual compilation.

### Output Format

```json
{
  "root": "/path/to/project",
  "srcDirs": ["src", "lib"],
  "useCached": false,
  "roots": ["Main"],
  "foreignModules": [
    {"name": "Html", "package": "elm/html"}
  ],
  "packageBuildOrder": [
    {
      "name": "elm/core",
      "version": "1.0.5",
      "path": "/home/user/.elm/0.19.1/packages/elm/core/1.0.5/src",
      "deps": []
    },
    {
      "name": "elm/json",
      "version": "1.1.3",
      "path": "/home/user/.elm/0.19.1/packages/elm/json/1.1.3/src",
      "deps": ["elm/core"]
    }
  ],
  "buildOrder": [
    {
      "name": "Utils",
      "path": "/path/to/project/src/Utils.elm",
      "deps": [],
      "hasMain": false,
      "cached": false
    },
    {
      "name": "Main",
      "path": "/path/to/project/src/Main.elm",
      "deps": ["Utils"],
      "hasMain": true,
      "cached": false
    }
  ],
  "parallelBatches": [
    {"level": 0, "count": 5, "modules": [...]},
    {"level": 1, "count": 3, "modules": [...]}
  ],
  "problems": [],
  "totalPackages": 15,
  "totalModules": 42,
  "modulesToBuild": 42,
  "parallelLevels": 8
}
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         cmd_build()                              │
├─────────────────────────────────────────────────────────────────┤
│  1. Parse arguments                                              │
│  2. Find project root (elm.json)                                 │
│  3. Read elm.json → ElmJson struct                               │
│  4. Resolve packages via PubGrub solver                          │
│  5. Compute package build order                                  │
│  6. Discover local modules                                       │
│  7. Parse imports, build dependency graph                        │
│  8. Compute module build order                                   │
│  9. Compute parallel batches                                     │
│ 10. Check cache status (if --use-cached)                         │
│ 11. Output JSON build plan                                       │
└─────────────────────────────────────────────────────────────────┘
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
   │  PubGrub    │    │   Module    │    │   Import    │
   │  Solver     │    │  Discovery  │    │   Parser    │
   │ (existing)  │    │             │    │             │
   └─────────────┘    └─────────────┘    └─────────────┘
          │                   │                   │
          ▼                   ▼                   ▼
   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
   │ Package     │    │  Topo Sort  │    │   Cache     │
   │ Graph       │    │  (SCC)      │    │  Detector   │
   └─────────────┘    └─────────────┘    └─────────────┘
```

---

## Data Structures

### Package Representation

```c
// In new file: src/build/build_types.h

typedef struct {
    char *author;           // e.g., "elm"
    char *name;             // e.g., "core"
    char *version;          // e.g., "1.0.5"
    char *src_path;         // Full path to src/ directory
    char **deps;            // Array of "author/name" strings
    int dep_count;
} BuildPackage;

typedef struct {
    BuildPackage *packages;
    int count;
    int capacity;
} BuildPackageList;
```

### Module Representation

```c
typedef struct {
    char *name;             // Module name, e.g., "Page.Home"
    char *path;             // Full path to .elm file
    char **deps;            // Array of module names (local deps only)
    int dep_count;
    bool has_main;          // Has `main` definition
    bool is_cached;         // Artifact is up-to-date
} BuildModule;

typedef struct {
    BuildModule *modules;
    int count;
    int capacity;
} BuildModuleList;
```

### Dependency Graph

```c
// Adjacency list representation for topological sort
typedef struct {
    char *name;             // Node identifier (package or module name)
    char **edges;           // Dependencies (outgoing edges)
    int edge_count;
    int edge_capacity;
    int in_degree;          // For Kahn's algorithm
    int level;              // For parallel batch computation
} GraphNode;

typedef struct {
    GraphNode *nodes;
    int count;
    int capacity;
    // Hash map for O(1) lookup by name
    // (Use existing hash map implementation or simple linear search for small graphs)
} DependencyGraph;
```

### Build Plan

```c
typedef struct {
    char *root;                     // Project root directory
    char **src_dirs;                // Source directories
    int src_dir_count;
    char **roots;                   // Entry point module names
    int root_count;
    bool use_cached;

    BuildPackageList packages;      // Packages in build order
    BuildModuleList modules;        // Modules in build order
    BuildModuleList foreign;        // Modules from packages

    // Parallel batches
    struct {
        int level;
        BuildModule *modules;
        int count;
    } *batches;
    int batch_count;

    // Problems encountered
    struct {
        char *module_name;
        char *error;
    } *problems;
    int problem_count;
} BuildPlan;
```

---

## Algorithms

### 1. Package Dependency Resolution

**Input:** elm.json with dependency declarations
**Output:** List of (author, name, version) tuples for all transitive dependencies

```
FUNCTION resolve_packages(elm_json):
    // Use existing PubGrub solver
    solver = pg_solver_new()

    FOR EACH (author, name, constraint) IN elm_json.dependencies:
        pg_solver_add_root_dependency(solver, author, name, constraint)

    result = pg_solver_solve(solver)

    IF result != PG_SUCCESS:
        RETURN error

    packages = []
    FOR EACH package_id IN solver.selected:
        version = pg_solver_get_selected_version(solver, package_id)
        packages.APPEND((author, name, version))

    RETURN packages
```

### 2. Package Build Order (Topological Sort)

**Input:** List of packages with versions
**Output:** Packages in dependency order (dependencies before dependents)

```
FUNCTION compute_package_build_order(packages, cache_path):
    // Build dependency graph by reading each package's elm.json
    graph = new DependencyGraph()

    FOR EACH pkg IN packages:
        pkg_elm_json_path = cache_path / pkg.author / pkg.name / pkg.version / "elm.json"
        pkg_elm_json = elm_json_read(pkg_elm_json_path)

        deps = []
        FOR EACH dep IN pkg_elm_json.dependencies:
            IF dep IN packages:  // Only include deps in our package set
                deps.APPEND(dep.author + "/" + dep.name)

        graph.add_node(pkg.author + "/" + pkg.name, deps)

    // Topological sort using Kahn's algorithm
    RETURN topological_sort(graph)
```

### 3. Topological Sort (Kahn's Algorithm)

```
FUNCTION topological_sort(graph):
    // Compute in-degrees
    FOR EACH node IN graph:
        node.in_degree = 0
    FOR EACH node IN graph:
        FOR EACH dep IN node.edges:
            graph.find(dep).in_degree += 1

    // Initialize queue with nodes having in_degree = 0
    queue = []
    FOR EACH node IN graph:
        IF node.in_degree == 0:
            queue.APPEND(node)

    result = []
    WHILE queue IS NOT EMPTY:
        node = queue.POP_FRONT()
        result.APPEND(node)

        // Decrease in-degree of neighbors
        FOR EACH neighbor_name IN node.edges:
            neighbor = graph.find(neighbor_name)
            neighbor.in_degree -= 1
            IF neighbor.in_degree == 0:
                queue.APPEND(neighbor)

    // Check for cycles
    IF result.length != graph.count:
        RETURN error("Cycle detected")

    RETURN result
```

### 4. Module Discovery

**Input:** Source directories from elm.json
**Output:** List of (module_name, file_path) pairs

```
FUNCTION discover_modules(root, src_dirs):
    modules = []

    FOR EACH src_dir IN src_dirs:
        abs_dir = root / src_dir
        elm_files = find_files_recursive(abs_dir, "*.elm")

        FOR EACH file_path IN elm_files:
            relative = file_path.relative_to(abs_dir)
            module_name = path_to_module_name(relative)
            // e.g., "Page/Home.elm" -> "Page.Home"

            modules.APPEND((module_name, file_path))

    RETURN modules

FUNCTION path_to_module_name(relative_path):
    // Remove .elm extension
    // Replace path separators with dots
    name = relative_path.remove_suffix(".elm")
    name = name.replace("/", ".")
    name = name.replace("\\", ".")
    RETURN name
```

### 5. Import Parsing

**Input:** Elm source file content
**Output:** List of imported module names, whether file has `main`

The import parser needs to handle:
- `import Module`
- `import Module as Alias`
- `import Module exposing (..)`
- `import Module exposing (foo, Bar(..))`
- Comments (line and block)
- Module declaration line

```
FUNCTION parse_imports(file_content):
    imports = []
    has_main = false

    // Tokenize and parse
    pos = 0
    WHILE pos < file_content.length:
        skip_whitespace_and_comments(pos)

        IF starts_with(file_content, pos, "import "):
            pos += 7
            module_name = parse_module_name(file_content, pos)
            imports.APPEND(module_name)
            skip_to_newline(pos)

        ELSE IF starts_with(file_content, pos, "module "):
            // Skip module declaration
            skip_to_newline(pos)

        ELSE IF at_top_level AND starts_with(file_content, pos, "main"):
            // Check if this is a main definition (not just a reference)
            IF is_main_definition(file_content, pos):
                has_main = true
            skip_to_newline(pos)

        ELSE:
            // Past imports section, can stop scanning for imports
            // (imports must come before any declarations)
            BREAK if past_import_section
            pos += 1

    RETURN (imports, has_main)

FUNCTION parse_module_name(content, pos):
    // Parse: UpperCase(.UpperCase)*
    parts = []
    WHILE true:
        part = parse_upper_identifier(content, pos)
        parts.APPEND(part)
        IF content[pos] != '.':
            BREAK
        pos += 1
    RETURN parts.join(".")
```

### 6. Module Build Order

```
FUNCTION compute_module_build_order(discovered_modules, src_dirs):
    // Parse all modules to get imports
    graph = new DependencyGraph()
    local_module_set = SET(m.name FOR m IN discovered_modules)
    foreign_modules = []

    FOR EACH (name, path) IN discovered_modules:
        content = file_read(path)
        (imports, has_main) = parse_imports(content)

        // Filter to local dependencies only
        local_deps = []
        FOR EACH imp IN imports:
            IF imp IN local_module_set:
                local_deps.APPEND(imp)
            ELSE:
                // Track foreign module for output
                foreign_modules.APPEND(imp)

        graph.add_node(name, local_deps, has_main, path)

    // Topological sort
    sorted = topological_sort(graph)

    RETURN (sorted, foreign_modules)
```

### 7. Parallel Batch Computation

Modules at the same "level" can be compiled in parallel because they have no dependencies on each other.

```
FUNCTION compute_parallel_batches(graph):
    // Compute level for each node
    // Level = max(level of dependencies) + 1
    // Nodes with no dependencies are level 0

    FOR EACH node IN graph:
        node.level = -1  // Uncomputed

    // Fixed-point iteration
    changed = true
    WHILE changed:
        changed = false
        FOR EACH node IN graph:
            IF node.edges.is_empty():
                IF node.level != 0:
                    node.level = 0
                    changed = true
            ELSE:
                // Check if all deps have levels computed
                all_deps_ready = true
                max_dep_level = -1
                FOR EACH dep_name IN node.edges:
                    dep = graph.find(dep_name)
                    IF dep.level == -1:
                        all_deps_ready = false
                        BREAK
                    max_dep_level = MAX(max_dep_level, dep.level)

                IF all_deps_ready:
                    new_level = max_dep_level + 1
                    IF node.level != new_level:
                        node.level = new_level
                        changed = true

    // Group by level
    max_level = MAX(node.level FOR node IN graph)
    batches = []
    FOR level FROM 0 TO max_level:
        batch = [node FOR node IN graph IF node.level == level]
        batches.APPEND(batch)

    RETURN batches
```

### 8. Cache Detection

For incremental builds (--use-cached), determine which modules need recompilation.

```
FUNCTION check_cache_status(module, elm_stuff_path):
    // Check if compiled artifact exists and is newer than source
    artifact_path = elm_stuff_path / "0.19.1" / "i.dat"
    // Note: Elm uses a single i.dat file with all interfaces

    // For more granular caching, check modification times
    source_mtime = file_mtime(module.path)

    // Check if any dependency was modified after last compile
    // This requires tracking the last compile timestamp
    last_compile = read_last_compile_time(module.name)

    IF last_compile == 0:
        RETURN false  // Never compiled

    IF source_mtime > last_compile:
        RETURN false  // Source modified

    FOR EACH dep_name IN module.deps:
        dep_compile_time = read_last_compile_time(dep_name)
        IF dep_compile_time > last_compile:
            RETURN false  // Dependency recompiled after us

    RETURN true  // Cached and up-to-date
```

---

## Integration Points

### 1. PubGrub Solver Integration

The existing solver in `src/pgsolver/` handles dependency resolution. Integration:

```c
#include "pgsolver/pg_elm.h"

// Create context with registry
PgElmContext *ctx = pg_elm_context_new(cache_config);

// Add dependencies from elm.json
for (int i = 0; i < elm_json->dependencies_direct->count; i++) {
    Package *pkg = &elm_json->dependencies_direct->packages[i];
    pg_elm_add_dependency(ctx, pkg->author, pkg->name, pkg->version);
}

// Solve
PgSolverResult result = pg_elm_solve(ctx);
if (result != PG_SUCCESS) {
    // Handle error
}

// Extract solution
int pkg_count;
PgSelectedPackage *selected = pg_elm_get_selected(ctx, &pkg_count);
```

### 2. elm.json Parsing

Use existing `src/elm_json.c`:

```c
#include "elm_json.h"

ElmJson *ej = elm_json_read(elm_json_path);
if (!ej) {
    log_error("Failed to read elm.json");
    return 1;
}

// Get source directories
if (ej->type == ELM_APPLICATION) {
    // Applications use source-directories field
    // Parse from elm.json directly
} else {
    // Packages always use "src" directory
}
```

### 3. File System Operations

Use existing `src/fileutil.h`:

```c
#include "fileutil.h"

// Read file with bounds checking
size_t size;
char *content = file_read_contents_bounded(path, MAX_ELM_FILE_SIZE, &size);

// Check file existence
if (!file_exists(path)) { ... }

// Recursive file discovery
void discover_elm_files(const char *dir, char ***files, int *count, int *capacity) {
    // Use elm_collect_elm_files() from elm_project.h
}
```

### 4. Cache Path Resolution

```c
#include "global_context.h"
#include "elm_paths.h"

// Get Elm home directory
const char *elm_home = get_elm_home();  // ~/.elm or ELM_HOME

// Get cache path for a package
char *pkg_path = arena_sprintf(arena,
    "%s/0.19.1/packages/%s/%s/%s",
    elm_home, author, name, version);
```

---

## Error Handling

### Error Types

```c
typedef enum {
    BUILD_OK = 0,
    BUILD_ERR_NO_ELM_JSON,
    BUILD_ERR_INVALID_ELM_JSON,
    BUILD_ERR_SOLVER_FAILED,
    BUILD_ERR_MISSING_PACKAGE,
    BUILD_ERR_FILE_NOT_FOUND,
    BUILD_ERR_PARSE_ERROR,
    BUILD_ERR_IMPORT_CYCLE,
    BUILD_ERR_INVALID_MODULE_NAME,
    BUILD_ERR_AMBIGUOUS_MODULE,
} BuildError;
```

### Error Reporting

```c
typedef struct {
    BuildError code;
    char *message;
    char *file_path;      // If applicable
    int line;             // If applicable
    int column;           // If applicable
} BuildProblem;

// Collect problems instead of failing immediately
void build_add_problem(BuildPlan *plan, BuildError code,
                       const char *fmt, ...);
```

### Detectable Error Conditions

The build plan phase can detect many errors before actual compilation, providing
better diagnostics than the Elm compiler's generic "You changed elm.json manually"
error. Here are the categories of errors detected:

#### 1. Package Resolution Errors (Solver Phase)

**When detected:** During `resolve_packages_v2()` before any modules are crawled.

| Error | Detection | User Message |
|-------|-----------|--------------|
| Non-existent package | Solver returns PG_NO_SOLUTION | "Package author/name does not exist" |
| Version not found | Solver returns PG_NO_SOLUTION | "No version of author/name satisfies constraint x.y.z <= v < x.y+1.0" |
| Incompatible constraints | Solver returns PG_NO_SOLUTION | "Dependency conflict: package-A requires elm/core 1.0.x but package-B requires elm/core 2.0.x" |
| Missing registry | Registry fetch fails | "Failed to fetch package registry" |

#### 2. elm.json Errors (Parse Phase)

**When detected:** During `elm_json_read()` before solving.

| Error | Detection | User Message |
|-------|-----------|--------------|
| Missing elm.json | File not found | "Could not find elm.json" |
| Invalid JSON | cJSON parse error | "Invalid JSON in elm.json" |
| Missing required field | Validation | "elm.json missing 'source-directories'" |
| Invalid package name | Validation | "Invalid package name: must be 'author/name'" |
| Invalid version | Validation | "Invalid version string: '1.0'" |

#### 3. Package Cache Errors

**When detected:** During `compute_package_build_order()`.

| Error | Detection | User Message |
|-------|-----------|--------------|
| Package not downloaded | Directory missing | "Package elm/core 1.0.5 not found in cache" |
| Corrupted package | elm.json unreadable | "Failed to read elm.json for elm/core 1.0.5" |
| Missing src directory | Directory missing | "Package elm/core 1.0.5 has no src directory" |

#### 4. Module Discovery Errors

**When detected:** During `crawl_modules()`.

| Error | Detection | User Message |
|-------|-----------|--------------|
| Entry file not found | File missing | "Entry file src/Main.elm not found" |
| Entry file not parseable | Parse fails | "Failed to parse entry file: src/Main.elm" |
| Missing local import | File not found | "Module Utils imported by Main but not found in src/" |
| Parse error in module | Tree-sitter error | "Syntax error in src/Page/Home.elm" |

#### 5. Module Dependency Errors

**When detected:** During `compute_module_build_order()`.

| Error | Detection | User Message |
|-------|-----------|--------------|
| Import cycle | DFS back-edge | "Circular import: Main → Page → Main" |
| Missing foreign module | Not in package exports | "Module Html.Extra not found in any package" |

#### 6. Source Directory Errors

**When detected:** During source directory resolution.

| Error | Detection | User Message |
|-------|-----------|--------------|
| Source dir not found | Directory missing | "Source directory 'src' does not exist" |
| Source dir outside project | Path validation | "Source directory '../other' is outside project root" |
| Invalid path | Path validation | "Invalid source directory path: 'src//double/slash'" |

### Error Display in `build check`

When `wrap build check` detects problems, it displays them prominently before
the build plan:

```
---- Build Plan --------------------------------------------------

Build plan for: `src/Main.elm`

  PROBLEMS DETECTED:
    - Utils: Module imported but not found in source directories
    - Page.Settings: Circular import detected

  Include cached data for 34 already built packages.

  ...

Cannot proceed with build due to problems above.
```

The command returns exit code 1 when problems are detected, even in `-n` mode,
allowing integration with CI systems.

---

## Implementation Phases

### Phase 1: Core Infrastructure (Week 1)

1. Create file structure:
   ```
   src/build/
   ├── build_types.h      # Data structures
   ├── build_driver.c     # Main entry point
   ├── build_driver.h
   ├── package_order.c    # Package dependency ordering
   ├── package_order.h
   └── Makefile.inc       # Build integration
   ```

2. Implement basic command skeleton:
   - Argument parsing
   - elm.json reading
   - Solver integration
   - JSON output scaffolding

3. Add to main.c command routing

### Phase 2: Package Build Order (Week 2)

1. Implement package elm.json reading from cache
2. Build package dependency graph
3. Implement topological sort
4. Generate packageBuildOrder JSON

### Phase 3: Module Discovery & Parsing (Week 3)

1. Implement recursive .elm file discovery
2. Implement import parser:
   - Handle all import syntax variants
   - Skip comments correctly
   - Detect main definition
3. Unit tests for parser edge cases

### Phase 4: Module Build Order (Week 4)

1. Build module dependency graph
2. Implement topological sort for modules
3. Separate local vs foreign modules
4. Handle ambiguous imports

### Phase 5: Parallel Batches & Cache (Week 5)

1. Implement level computation algorithm
2. Group modules into batches
3. Implement cache status detection
4. Add --use-cached flag support

### Phase 6: Polish & Testing (Week 6)

1. Comprehensive error messages
2. Integration tests
3. Performance testing on large projects
4. Documentation

---

## Testing Strategy

### Unit Tests

```c
// test/build/test_import_parser.c

void test_simple_import(void) {
    const char *src = "import Html";
    ImportResult r = parse_imports(src);
    assert(r.count == 1);
    assert(strcmp(r.imports[0], "Html") == 0);
}

void test_qualified_import(void) {
    const char *src = "import Html.Attributes as Attr";
    ImportResult r = parse_imports(src);
    assert(r.count == 1);
    assert(strcmp(r.imports[0], "Html.Attributes") == 0);
}

void test_import_with_exposing(void) {
    const char *src = "import Html exposing (div, text)";
    // ...
}

void test_multiline_exposing(void) {
    const char *src =
        "import Html exposing\n"
        "    ( div\n"
        "    , text\n"
        "    )";
    // ...
}

void test_comments_before_import(void) {
    const char *src =
        "-- This is a comment\n"
        "{- Block comment -}\n"
        "import Html";
    // ...
}

void test_main_detection(void) {
    const char *src =
        "module Main exposing (main)\n"
        "import Html\n"
        "main = Html.text \"Hello\"";
    ImportResult r = parse_imports(src);
    assert(r.has_main == true);
}
```

### Integration Tests

```bash
# test/build/integration/test_basic_app.sh

# Create test project
mkdir -p test_project/src
cat > test_project/elm.json << 'EOF'
{
    "type": "application",
    "source-directories": ["src"],
    "dependencies": {
        "direct": {
            "elm/core": "1.0.5",
            "elm/html": "1.0.0"
        },
        "indirect": {
            "elm/json": "1.1.3",
            "elm/virtual-dom": "1.0.3"
        }
    }
}
EOF

cat > test_project/src/Main.elm << 'EOF'
module Main exposing (main)
import Html
main = Html.text "Hello"
EOF

# Run build command
cd test_project
wrap build src/Main.elm > build_plan.json

# Verify output
jq -e '.packageBuildOrder | length > 0' build_plan.json
jq -e '.buildOrder | length == 1' build_plan.json
jq -e '.buildOrder[0].name == "Main"' build_plan.json
jq -e '.buildOrder[0].hasMain == true' build_plan.json
```

### Golden Tests

Compare output against reference elm-build tool:

```bash
# test/build/golden/run_golden_tests.sh

for project in test/build/golden/projects/*; do
    echo "Testing $project..."

    # Generate with our tool
    wrap build "$project/src/Main.elm" > /tmp/wrap_output.json

    # Generate with reference (elm-build from Haskell)
    elm-build "$project/src/Main.elm" > /tmp/ref_output.json

    # Compare (ignoring path differences)
    normalize_paths /tmp/wrap_output.json > /tmp/wrap_norm.json
    normalize_paths /tmp/ref_output.json > /tmp/ref_norm.json

    diff /tmp/wrap_norm.json /tmp/ref_norm.json || exit 1
done
```

---

## Performance Considerations

### Memory Management

- Use arena allocator for all allocations (per AGENTS.md)
- Single arena per build invocation
- No need to free individual allocations

### File I/O

- Use bounded file reads (`file_read_contents_bounded`)
- Consider memory-mapping for large files
- Batch directory traversal where possible

### Graph Operations

- For small-medium projects (<500 modules): Simple adjacency list is fine
- For large projects: Consider hash map for O(1) node lookup
- Topological sort is O(V + E), dominated by file I/O anyway

### Parallelism

The build plan enables external parallelism:
- Parallel batch info allows build systems to parallelize compilation
- This tool itself is single-threaded (simpler, sufficient for plan generation)

---

## Security Considerations

Per `doc/writing-secure-code.md` and AGENTS.md:

1. **Bounded reads**: All file reads use `file_read_contents_bounded()`
2. **Path validation**: Validate paths don't escape project root
3. **Arena allocator**: No manual memory management
4. **Input limits**:
   - MAX_ELM_FILE_SIZE for source files
   - MAX_IMPORT_COUNT per file
   - MAX_MODULE_COUNT per project
5. **No shell execution**: Don't shell out for any operations

---

## Constants

Add to `src/constants.h`:

```c
// Build driver limits
#define MAX_ELM_FILE_SIZE        (10 * 1024 * 1024)  // 10 MB per file
#define MAX_IMPORT_COUNT         1000                 // Imports per file
#define MAX_MODULE_COUNT         10000                // Modules per project
#define MAX_PACKAGE_COUNT        500                  // Packages per project
#define MAX_MODULE_NAME_LENGTH   256
#define MAX_PATH_LENGTH          4096
```

---

## File Layout

```
src/
├── build/
│   ├── build_types.h           # Data structures
│   ├── build_driver.c          # Main entry, JSON output
│   ├── build_driver.h          # Public API
│   ├── package_order.c         # Package dependency graph
│   ├── package_order.h
│   ├── module_discovery.c      # Find .elm files
│   ├── module_discovery.h
│   ├── import_parser.c         # Parse imports from source
│   ├── import_parser.h
│   ├── topo_sort.c             # Topological sort algorithm
│   ├── topo_sort.h
│   ├── parallel_batches.c      # Level computation
│   ├── parallel_batches.h
│   ├── cache_detect.c          # Incremental build support
│   └── cache_detect.h
├── commands/
│   └── wrappers/
│       └── build.c             # Command handler (thin wrapper)
└── main.c                      # Add "build" command routing
```

---

## Open Questions

1. **Kernel modules**: How to handle `elm/` kernel modules (JS interop)?
   - They don't have .elm source, only .js
   - Likely: exclude from module graph, treat as "always available"

2. **Effect managers**: Special handling for `Platform.*` modules?
   - Likely: No special handling needed for build order

3. **Test dependencies**: Include test-dependencies in build plan?
   - Option: `--include-tests` flag

4. **Docs generation**: Should build plan include info for docs?
   - Likely: Separate command or flag

5. **Source map support**: Track line/column info for error messages?
   - Phase 2 enhancement

---

## Appendix: Elm Import Syntax Reference

```elm
-- Simple import
import Html

-- Qualified import with alias
import Html.Attributes as Attr

-- Import with exposing
import Html exposing (div, text)

-- Import exposing all
import Html exposing (..)

-- Import with type exposing variants
import Maybe exposing (Maybe(..))

-- Multiline exposing
import Html exposing
    ( div
    , span
    , text
    )

-- Nested module names
import Json.Decode.Pipeline

-- Comments can appear anywhere
import Html -- inline comment
{- block comment -} import Json.Decode
```

### Module Declaration

Must be first non-comment line:
```elm
module Main exposing (main)
module Page.Home exposing (..)
port module Ports exposing (sendMessage)
effect module Task exposing (Task, perform)
```

### Main Detection

A module has `main` if it contains a top-level binding:
```elm
main = ...
main : Html msg
main = ...
```

Note: `main` must be exposed for it to be a true entry point.
