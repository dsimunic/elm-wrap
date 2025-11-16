# Install Command Summary

## Implementation Features

### Dependency Promotion ✅

The implementation correctly handles all promotion scenarios:

- **Indirect → Direct**: When a package exists in indirect dependencies and user installs it
- **Test → Direct**: When a package exists in test dependencies and user installs it (non-test)
- **Test Indirect → Test Direct**: When installing with `--test` flag

Example output:
```
$ elm-wrap install elm/json
Package elm/json is already in your dependencies.
Promoted elm/json from indirect to direct dependencies.
```

### Elm.json Reading ✅

- Parses project type (application vs package)
- Loads all dependency maps (direct, indirect, test-direct, test-indirect)
- Currently stubbed with sample data (TODO: implement JSON parser)

### Cache Management ✅

- Respects `ELM_HOME` environment variable
- Uses default `~/.elm` if not set
- Constructs proper cache paths: `$ELM_HOME/0.19.1/packages/author/package/version/`
- Checks package existence before downloading

### Solver Integration ✅

- Initializes solver with cache and online status
- Queries registry for available versions (stubbed)
- Selects compatible version (currently newest)
- Adds package to appropriate dependency map
- Reports solver errors appropriately

## Elm Compiler Install Algorithm

Based on analysis of the Haskell implementation in `terminal/src/Install.hs` and `builder/src/Deps/Solver.hs`, here's the complete install algorithm that needs to be implemented in C:

### High-Level Flow

1. **Parse Command Line**: Extract package name (author/package format)
2. **Find Project Root**: Locate `elm.json` file
3. **Initialize Environment**: Set up cache, HTTP manager, and registry
4. **Read Outline**: Parse `elm.json` to determine project type and current dependencies
5. **Check Existing Dependencies**: Handle promotion cases or proceed to solver
6. **Run Solver**: Find compatible package versions and transitive dependencies
7. **Update Outline**: Modify `elm.json` with new/changed dependencies
8. **Verify Installation**: Download packages and verify everything works

### Detailed Algorithm

#### Step 1: Initialize Environment
- Get package cache location (`$ELM_HOME/0.19.1/packages/`)
- Initialize HTTP manager for downloads
- Load or fetch package registry:
  - Try to read cached registry from `$ELM_HOME/0.19.1/packages/databases/registry.dat`
  - If no cached registry: Download from `https://package.elm-lang.org/all-packages`
  - If cached registry exists: Check for updates via `https://package.elm-lang.org/all-packages/since/{count}`
  - Parse JSON response into registry map (package name → available versions)
- Determine connection mode (online/offline) based on network availability

#### Step 2: Read elm.json Outline
- Parse JSON to determine if it's an application or package project
- Extract dependency maps:
  - `direct`: Direct dependencies (runtime)
  - `indirect`: Indirect dependencies (transitive)
  - `test-direct`: Test-only direct dependencies
  - `test-indirect`: Test-only indirect dependencies

#### Step 3: Check for Existing Package
For application projects (`Outline.App`):
1. If package exists in `direct` → Return "Already installed"
2. If package exists in `indirect` → Promote to `direct` (remove from `indirect`)
3. If package exists in `test-direct` → Promote to `direct` (remove from `test-direct`)
4. If package exists in `test-indirect` → Promote to `direct` (remove from `test-indirect`)
5. Otherwise → Check registry for package existence, then proceed to solver

For package projects (`Outline.Pkg`):
1. If package exists in `deps` → Return "Already installed"
2. If package exists in `test` → Promote to `deps` (remove from `test`)
3. Otherwise → Check registry for package existence, then proceed to solver

**Registry Check:**
- Query registry for package name
- If not found: Return error with suggestions for similar package names
- If found: Proceed to solver with available versions

#### Step 3: Dependency Resolution (Solver)
The solver uses a backtracking algorithm with multiple strategies:

**Initialization:**
- Combine all existing dependencies into constraint map
- Add target package with `C.anything` constraint
- Initialize solved packages as empty

**Resolution Strategies (tried in order):**
1. `C.exactly` on all existing deps (preserve exact versions)
2. `C.exactly` on direct deps only
3. `C.untilNextMinor` on direct deps
4. `C.untilNextMajor` on direct deps
5. `C.anything` on direct deps (most permissive)

**For each strategy:**
- Start with pending constraints (target package + existing)
- Iteratively solve each package:
  - Get available versions from registry that satisfy constraint
  - Try newest version first, then older versions on backtrack
  - For each version, fetch its `elm.json` to get dependencies
  - Add new dependencies to pending constraints
  - Check for constraint conflicts
  - Recursively solve transitive dependencies

**Constraint Types:**
- `C.exactly v`: Exactly version v
- `C.untilNextMinor v`: >= v, < next minor (e.g., >=1.2.3, <1.3.0)
- `C.untilNextMajor v`: >= v, < next major (e.g., >=1.2.3, <2.0.0)
- `C.anything`: Any version

#### Step 4: Calculate Changes
- Compare old dependency maps with new solved maps
- Detect additions, changes, and removals
- For applications: Separate into direct/indirect/test-direct/test-indirect
- For packages: Separate into deps/test-deps

#### Step 5: Update elm.json
- Write new dependency maps back to JSON
- Preserve all other fields (elm version, source dirs, etc.)

#### Step 6: Verify Installation
- For each new/changed package:
  - Check if package exists in cache (`$ELM_HOME/0.19.1/packages/author/package/version/`)
  - If not cached and online: Download from package website
  - If offline: Fail if not cached
  - Verify package structure (elm.json, src/ directory)
- Ensure all transitive dependencies are available
- Validate no circular dependencies

### Key Data Structures

**Solver State:**
- `pending`: Map of package names to constraints (unsolved)
- `solved`: Map of package names to versions (solved)
- `constraints_cache`: Map of (pkg,version) to package constraints

**Package Constraints:**
- `elm`: Elm version constraint
- `deps`: Map of dependency names to version constraints

**Registry Data:**
- Available versions for each package
- Downloaded from `https://package.elm-lang.org/all-packages` (full) or `https://package.elm-lang.org/all-packages/since/{count}` (incremental)
- Cached locally in `$ELM_HOME/0.19.1/packages/databases/registry.dat` (binary format)
- JSON format: `{"author/package": ["1.0.0", "1.0.1", ...], ...}`

### Error Handling
- **Registry Fetch Failed**: Cannot download/update package registry
- **Unknown Package**: Package not found in registry (with similar name suggestions)
- **No Solution**: No version satisfies all constraints
- **Network Errors**: Failed to download packages/metadata
- **Cache Corruption**: Invalid package data in cache
- **JSON Parse Errors**: Malformed elm.json files or registry data
