# Build Ordering Algorithm

This document describes the module discovery and build ordering algorithms used by the `wrap build` command.

## Overview

The `wrap build` command generates a JSON build plan for Elm compilation. Given one or more entry point files (e.g., `src/Main.elm`), it produces:

1. A list of all local modules that need to be compiled
2. A topologically sorted build order (dependencies before dependents)
3. Foreign module mappings (which package provides each imported module)
4. Package build order
5. Parallel batching information for concurrent compilation

## Module Discovery

### Reachability-Based Crawling

Unlike scanning all `.elm` files in source directories, the build command uses **reachability-based crawling** starting from entry points. This matches the Elm compiler's `Build.crawlProject` behavior.

```
Algorithm: BFS Module Crawl
Input: Entry point files, source directories
Output: Set of reachable local modules

1. Parse each entry point to get its module name
2. Add entry points to work queue
3. While queue is not empty:
   a. Dequeue module name
   b. Find corresponding .elm file in source directories
   c. Parse file to extract imports
   d. For each import:
      - If local (file exists in source dirs): add to queue if not seen
      - If foreign: record in foreign modules list
   e. Add module to discovered set with its metadata
```

This approach ensures only modules actually reachable from entry points are included, avoiding compilation of unused code.

### Implicit Core Imports

Elm implicitly imports several core modules. These are added to the foreign modules list:

- `Basics`
- `Char`
- `Debug`
- `Maybe`
- `Platform`
- `Platform.Cmd`
- `Platform.Sub`
- `Tuple`

### Module-to-Package Mapping

Foreign modules must be mapped to their source packages. This is done by reading the `exposed-modules` field from each package's `elm.json`:

```
Algorithm: Build Module-Package Map
Input: List of packages with versions
Output: Map from module name to package name

For each package:
  1. Read package's elm.json from cache
  2. Parse exposed-modules (array or categorized object)
  3. For each exposed module name:
     Add mapping: module_name -> package_name
```

## Topological Sort

### Algorithm: DFS Post-Order

The build order uses depth-first search with post-order collection. This ensures dependencies are compiled before the modules that depend on them.

```
Algorithm: DFS Topological Sort
Input: Modules sorted alphabetically, with dependency lists
Output: Modules in topological order

1. Sort all modules alphabetically by name
2. Sort each module's dependency list alphabetically
3. For each module (in alphabetical order):
   If not visited:
     DFS_Visit(module)

DFS_Visit(module):
  1. Mark module as visited
  2. For each dependency (in alphabetical order):
     If not visited: DFS_Visit(dependency)
  3. Append module to output (post-order)
```

### Comparison with Haskell's stronglyConnComp

The reference implementation uses Haskell's `Data.Graph.stronglyConnComp`, which:

1. Builds a graph with vertices numbered by sorted key order
2. Computes topological sort via reverse post-order DFS
3. Returns strongly connected components in dependency order

For a DAG (which module dependencies form), each SCC contains exactly one module, and the output is a valid topological order.

**Key difference**: The exact ordering of modules at the same "level" (same dependency depth) may differ between implementations due to:

- DFS traversal order variations
- Tie-breaking rules when multiple modules are ready simultaneously

Both orderings are **valid topological orders** - all dependencies appear before their dependents. The difference only affects which independent modules are processed first during parallel compilation.

### Ordering Guarantees

The implementation guarantees:

1. **Correctness**: Every module's dependencies appear before it in the output
2. **Determinism**: Same input always produces same output
3. **Main last**: Entry point modules with most dependencies appear last

## Package Build Order

Packages are also topologically sorted based on their inter-package dependencies:

```
Algorithm: Package Topological Sort
Input: Packages from elm.json dependencies
Output: Packages in dependency order

1. Read each package's elm.json to get its dependencies
2. Filter to only include deps in our package set
3. Apply Kahn's algorithm with alphabetical tie-breaking:
   - Compute in-degrees for all packages
   - Repeatedly select package with in-degree 0 (alphabetically first if tie)
   - Decrease in-degrees of dependent packages
```

## Parallel Batching

Modules are grouped into parallel batches based on their dependency depth:

```
Algorithm: Compute Parallel Levels
Input: Modules with dependencies
Output: Level assignment for each module

For each module:
  If no dependencies: level = 0
  Else: level = max(dependency levels) + 1

Group modules by level into batches.
```

Modules in the same batch can be compiled in parallel since they have no dependencies on each other.

## Output Format

The JSON build plan includes:

```json
{
  "root": "/path/to/project",
  "srcDirs": ["/absolute/path/to/src", ...],
  "useCached": false,
  "roots": ["Main"],
  "foreignModules": [
    {"name": "Html", "package": "elm/html"},
    ...
  ],
  "packageBuildOrder": [
    {"name": "elm/core", "version": "1.0.5", "path": "...", "deps": []},
    ...
  ],
  "buildOrder": [
    {"name": "Utils", "path": "src/Utils.elm", "deps": [], "hasMain": false, "cached": false},
    ...
  ],
  "parallelBatches": [
    {"level": 0, "count": 5, "modules": [...]},
    ...
  ],
  "problems": [],
  "totalPackages": 58,
  "totalModules": 199,
  "modulesToBuild": 199,
  "parallelLevels": 15
}
```

## Implementation Files

- `src/build/build_driver.c` - Main build plan generation logic
- `src/build/build_driver.h` - Public API
- `src/build/build_types.h` - Data structures
- `src/commands/wrappers/build.c` - Command entry point

## References

- Elm compiler's `Build.crawlProject` for module discovery
- Haskell's `Data.Graph.stronglyConnComp` for topological sort
- Kahn's algorithm for package ordering
