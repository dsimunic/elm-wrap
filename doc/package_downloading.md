# Elm Package Download/Caching Mechanism

This document describes how the Elm compiler handles package downloading, caching, and dependency resolution for the `elm install`, `elm make`, `elm init`, `elm diff`, and `elm publish` commands.

## Overview

Elm packages are cached locally in the user's Elm home directory (`$ELM_HOME/0.19.1/packages`), organized by author, package name, and version. The package registry is maintained centrally at `https://package.elm-lang.org` and cached locally.

## Package Registry

- **Location**: `https://package.elm-lang.org/all-packages`
- **Local Cache**: `$ELM_HOME/<elm-version>/packages/registry.dat` (for example, `$ELM_HOME/0.19.1/packages/registry.dat`)
- **Structure**: Maps package names to `KnownVersions` containing the latest version and previous versions
- **Update Process**: Fetched on first use or when outdated; stored as binary data

`elm-wrap` relies on this versioned layout of `ELM_HOME` to ensure that each
compiler version sees only package versions that are compatible with that Elm
release. The dependency solver (including major upgrade strategies) operates
under the assumption that the active `registry.dat` has already been filtered
to Elm-compatible packages for the current compiler.

## Package Downloads

Packages are downloaded as ZIP archives from the Elm package website:

- **Metadata URL**: `https://package.elm-lang.org/packages/{author}/{package}/{version}/endpoint.json`
- **Archive URL**: Specified in `endpoint.json` with expected SHA1 hash
- **Local Path**: `$ELM_HOME/0.19.1/packages/{author}/{package}/{version}/`
- **Verification**: Archive hash is verified against expected hash before extraction

Key files involved:
- `builder/src/Elm/Details.hs`: `downloadPackage` function handles the download and extraction
- `builder/src/Deps/Website.hs`: Defines package URLs
- `builder/src/Stuff.hs`: Defines cache directory structure

## Dependency Resolution

Dependency resolution uses a constraint solver that attempts multiple strategies:

1. Exact versions of direct dependencies
2. Exact versions of all dependencies (direct + indirect)
3. Until next minor version for direct dependencies
4. Until next major version for direct dependencies
5. Any version for direct dependencies

Key files:
- `builder/src/Deps/Solver.hs`: Core solver implementation
- `terminal/src/Install.hs`: `makeAppPlan` and `makePkgPlan` create dependency plans

## Command-Specific Behavior

### `elm install`

**Process**:
1. Reads `elm.json` to get current dependencies
2. If package already exists in dependencies, promotes it (indirect → direct, test → direct)
3. Otherwise, uses solver to find compatible version
4. Updates `elm.json` with new dependencies
5. Calls `Details.verifyInstall` which downloads missing packages

**Key Code**: `terminal/src/Install.hs` - `run`, `makeAppPlan`, `attemptChanges`

### `elm make`

**Download Behavior**: Downloads missing packages automatically during build process.

**Process**:
1. Calls `Details.load` which verifies all dependencies
2. `Details.verifyApp` / `Details.verifyPkg` ensure all packages are available
3. `Details.verifyDependencies` checks cache and downloads missing packages via `downloadPackage`

**Key Code**: `terminal/src/Make.hs` - `runHelp` calls `Details.load`

### `elm init`

**Download Behavior**: Downloads default core packages during project initialization.

**Process**:
1. Creates `elm.json` with default dependencies (core packages)
2. Calls `Solver.verify` which downloads packages if not cached
3. Creates `src/` directory

**Key Code**: `terminal/src/Init.hs` - `init` function

### `elm diff`

**Download Behavior**: Downloads package documentation as needed for comparison.

**Process**:
1. Fetches documentation JSON for specified versions
2. Downloads `docs.json` from package website if not cached locally
3. Compares API changes between versions

**Key Code**: 
- `terminal/src/Diff.hs` - `getDocs`
- `builder/src/Deps/Diff.hs` - `getDocs` downloads docs.json

### `elm publish`

**Status**: Publishing is disabled in this version of the Elm compiler (0.19.1). The command exists but exits with a message indicating publishing is not supported.

**Key Code**: `terminal/src/Publish.hs` - Simplified stub implementation

## Cache Structure

```
$ELM_HOME/0.19.1/
├── packages/
│   ├── registry.dat          # Cached package registry
│   ├── author1/
│   │   └── package1/
│   │       └── 1.0.0/        # Extracted package contents
│   │           ├── src/
│   │           ├── elm.json
│   │           ├── docs.json
│   │           └── artifacts.dat  # Cached compilation artifacts
│   └── author2/
│       └── package2/
│           └── 2.1.0/
└── repl/                     # REPL-specific cache
```

## Key Functions and Modules

- **Solver.initEnv** (`builder/src/Deps/Solver.hs`): Initializes solver environment with cache, HTTP manager, and registry
- **Details.verifyInstall** (`builder/src/Elm/Details.hs`): Downloads and verifies packages after dependency changes
- **Details.verifyDependencies** (`builder/src/Elm/Details.hs`): Ensures all dependencies are downloaded and compiled
- **downloadPackage** (`builder/src/Elm/Details.hs`): Downloads and extracts individual packages
- **Registry.fetch** (`builder/src/Deps/Registry.hs`): Downloads the package registry
- **Stuff.package** (`builder/src/Stuff.hs`): Constructs local package cache paths

## Error Handling

- Network failures during download
- Hash verification failures
- Registry fetch failures
- Constraint solver failures (no compatible versions found)
- Corrupted cache files

All download operations use the `Http` module for network requests with proper error reporting.