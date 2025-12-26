# Package artifacts.dat Format

This document describes the `artifacts.dat` file format used by the Elm compiler for caching compiled package artifacts.

## Overview

The Elm compiler caches compiled package artifacts to avoid recompilation when the same package is used across different projects or when dependencies haven't changed. Each package in the Elm package cache can have an `artifacts.dat` file containing pre-compiled interfaces and optimized AST.

## File Location

Package artifacts are stored at:

```
~/.elm/<compiler-version>/packages/<author>/<name>/<version>/artifacts.dat
```

For example:
```
~/.elm/0.19.1/packages/elm/core/1.0.5/artifacts.dat
~/.elm/0.19.1/packages/elm/html/1.0.0/artifacts.dat
```

## Binary Format

The `artifacts.dat` file uses Haskell's `Data.Binary` serialization format. The top-level structure is:

```haskell
data ArtifactCache =
  ArtifactCache
    { _fingerprints :: Set.Set Fingerprint
    , _artifacts :: Artifacts
    }
```

### Fingerprints

```haskell
type Fingerprint = Map.Map Pkg.Name V.Version
```

A fingerprint is a snapshot of the exact dependency versions used when compiling the package. Since a package like `elm/html` depends on `elm/core`, `elm/json`, and `elm/virtual-dom`, its fingerprint records the specific versions of those dependencies:

```
{ "elm/core" -> 1.0.5
, "elm/json" -> 1.1.3
, "elm/virtual-dom" -> 1.0.3
}
```

The `_fingerprints` field is a **set** of fingerprints, allowing the same `artifacts.dat` to cache compiled artifacts for multiple dependency version combinations. This is useful when the same package is used in different projects with slightly different dependency trees.

### Artifacts

```haskell
type Artifacts = (Interfaces, Opt.GlobalGraph)

type Interfaces = Map.Map ModuleName.Canonical I.DependencyInterface
```

The artifacts contain:

1. **Interfaces**: A map from module names to their public API interfaces. Each interface includes:
   - Exposed values and their type signatures
   - Type alias definitions
   - Custom type (union) definitions
   - Binary operator definitions

2. **GlobalGraph**: The optimized AST ready for code generation, containing:
   - Function definitions as optimized expression trees
   - Field index mappings for record access

## How Caching Works

When the Elm compiler needs to compile a package:

1. **Check for existing artifacts.dat**: If the file exists, read it
2. **Compare fingerprints**: Check if any fingerprint in the set matches the current project's dependency versions for this package's dependencies
3. **Cache hit**: If a matching fingerprint exists, use the cached interfaces and objects
4. **Cache miss**: If no matching fingerprint, compile the package and add a new fingerprint entry

### Fingerprint Matching

A fingerprint matches if for every package in the fingerprint map, the current project uses the same version. This ensures that:

- API compatibility is maintained (interfaces compiled against `elm/core@1.0.5` work with current `elm/core@1.0.5`)
- Optimizations are valid (object code assumes specific dependency behavior)

## Project-Level Artifacts

In addition to package-level `artifacts.dat`, projects store compilation state in `elm-stuff/<version>/`:

| File | Contents |
|------|----------|
| `d.dat` | Details: module metadata, timestamps, build IDs |
| `i.dat` | Interfaces: cached public APIs for all modules |
| `o.dat` | Objects: compiled optimized AST for all modules |

Per-module artifacts are also stored:
- `<Module-Name>.elmi` - Individual module interface
- `<Module-Name>.elmo` - Individual module object code

## Source References

From the Elm compiler source:

- **Path definitions**: `builder/src/Stuff.hs` lines 42-64
- **ArtifactCache type**: `builder/src/Elm/Details.hs` lines 413-421
- **Binary instances**: `builder/src/Elm/Details.hs` lines 830-832
- **Interface type**: `compiler/src/Elm/Interface.hs` lines 36-43
- **GlobalGraph type**: `compiler/src/AST/Optimized.hs` lines 127-139

## Checking Artifact Status

For build planning purposes, we check the status of each package's `artifacts.dat`:

- **present**: File exists and contains a fingerprint that matches the current project's dependency versions
- **stale**: File exists but no fingerprint matches (artifacts were compiled against different dependency versions)
- **missing**: File does not exist (package needs compilation)

### Binary Parsing Details

The `wrap build` command parses `artifacts.dat` files to validate fingerprints:

1. **Read Set size** (8 bytes, big-endian): Number of fingerprints in the set
2. **For each fingerprint**:
   - Read Map size (8 bytes, big-endian): Number of entries
   - For each entry:
     - Read author name: 1-byte length prefix + string
     - Read project name: 1-byte length prefix + string
     - Read version: 3 bytes (compact format: `major << 16 | minor << 8 | patch`)
3. **Build expected fingerprint**: Map of this package's dependencies to their versions in the current project
4. **Match check**: A stored fingerprint matches if every entry in expected has the same version in stored

### Implementation

- **C implementation**: `src/build/build_driver.c` - `parse_artifact_fingerprints()`, `check_package_artifact_status()`
- **Haskell gold standard**: `foreign/Elm-Compiler/build/Main.hs` - `checkArtifactStatus()`, `parseArtifactFingerprints()`
