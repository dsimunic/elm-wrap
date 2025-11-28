# Elm Package Solver Analysis and C Re-implementation Plan

## Overview

The Elm compiler's package dependency solver is implemented in Haskell in `builder/src/Deps/Solver.hs`. It uses a backtracking constraint satisfaction algorithm to resolve package dependencies for Elm projects. The solver handles version constraints, transitive dependencies, and multiple resolution strategies.

## Solver Architecture

### Core Components

1. **Solver Monad**: A custom monad that encapsulates backtracking logic
2. **State**: Contains cache, connection status, registry, and constraint cache
3. **Goals**: Pending packages to resolve and solved packages
4. **Constraints**: Version constraints and dependency requirements

### Key Data Structures

#### Solver Monad
```haskell
newtype Solver a = Solver (forall b. State -> (State -> a -> (State -> IO b) -> IO b) -> (State -> IO b) -> (Exit.Solver -> IO b) -> IO b)
```

#### State
```haskell
data State = State {
  _cache :: Stuff.PackageCache,
  _connection :: Connection,
  _registry :: Registry.Registry,
  _constraints :: Map.Map (Pkg.Name, V.Version) Constraints
}
```

#### Constraints
```haskell
data Constraints = Constraints {
  _elm :: C.Constraint,
  _deps :: Map.Map Pkg.Name C.Constraint
}
```

#### Goals
```haskell
data Goals = Goals {
  _pending :: Map.Map Pkg.Name C.Constraint,
  _solved :: Map.Map Pkg.Name V.Version
}
```

## Algorithm Flow

### Main Resolution Process

1. **Initialize**: Start with direct dependencies as pending goals
2. **Explore Goals**: Process each pending package constraint
3. **Get Versions**: Find versions that satisfy the constraint
4. **Try Versions**: Attempt to add each version to the solution
5. **Add Constraints**: Include dependency constraints from the chosen version
6. **Check Conflicts**: Verify no constraint violations
7. **Backtrack**: If conflicts occur, try alternative versions
8. **Repeat**: Continue until all goals are resolved or no solution exists

### Resolution Strategies

The solver employs multiple strategies in order of preference:

1. **Exact versions** of all dependencies (direct + indirect)
2. **Exact versions** of direct dependencies only
3. **Until next minor** version for direct dependencies
4. **Until next major** version for direct dependencies
5. **Any version** for direct dependencies

For Elm application projects, `elm-wrap` mirrors this ladder in its C
implementation. In particular, for the most permissive "any version" / major
upgrade strategy, the client-side solver does not add additional elm-version
constraints for individual packages. Instead, it relies on the registry
and local cache layout (versioned `$ELM_HOME/<elm-version>/packages/registry.dat`)
to expose only package versions that are compatible with the currently running
Elm compiler. As a result, the solver's search space already consists solely of
Elm-compatible versions when performing major upgrades.

### Backtracking Mechanism

- Uses continuation-passing style for backtracking
- `oneOf` function tries alternatives in sequence
- `backtrack` function abandons current path and tries next alternative

## Key Functions

### Core Solver Functions

- `try`: Initiates solving for a set of constraints
- `exploreGoals`: Main recursive goal exploration
- `addVersion`: Attempts to add a package version to the solution
- `addConstraint`: Merges new constraints with existing ones
- `getRelevantVersions`: Filters versions satisfying a constraint
- `getConstraints`: Retrieves package constraints from cache or network

### Registry Interaction

- `Registry.getVersions`: Retrieves available versions for a package
- `Registry.fetch`/`Registry.update`: Downloads/updates package registry
- Registry cached locally as binary data

## C Re-implementation Plan

### Data Structures

#### Core Types
```c
typedef struct {
    // Package cache path
    char* cache_dir;
    // HTTP connection status
    bool online;
    // Registry data
    Registry* registry;
    // Constraint cache: Map<PackageVersion, Constraints>
    HashMap* constraint_cache;
} SolverState;

typedef struct {
    // Version constraint
    Constraint* elm_constraint;
    // Dependency constraints: Map<PackageName, Constraint>
    HashMap* dependencies;
} PackageConstraints;

typedef struct {
    // Pending packages: Map<PackageName, Constraint>
    HashMap* pending;
    // Solved packages: Map<PackageName, Version>
    HashMap* solved;
} Goals;

typedef struct {
    // Package name
    PackageName name;
    // Version
    Version version;
} PackageVersion;
```

#### Collections
- Use hash maps for name-to-constraint/version mappings
- Dynamic arrays for version lists
- String interning for package names

### Algorithm Implementation

#### Main Solver Loop
```c
SolverResult* solve(SolverState* state, HashMap* initial_constraints) {
    Goals* goals = create_goals(initial_constraints);
    
    while (has_pending_goals(goals)) {
        PackageName* pkg = get_next_pending(goals);
        Constraint* constraint = get_constraint(goals, pkg);
        
        VersionList* versions = get_relevant_versions(state, pkg, constraint);
        if (versions->count == 0) {
            return NO_SOLUTION;
        }
        
        bool found = false;
        for (int i = 0; i < versions->count; i++) {
            if (try_add_version(state, goals, pkg, versions->versions[i])) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            return NO_SOLUTION;
        }
    }
    
    return create_solution(goals);
}
```

#### Backtracking Strategy
- Use recursive function calls with state snapshots
- Implement `oneOf` as sequential attempts with cleanup
- Maintain solution stack for backtracking

#### Version Selection
- Implement constraint satisfaction checking
- Support range constraints (exact, until-next-minor, etc.)
- Semantic versioning comparison functions

### Registry Management

#### Registry Structure
```c
typedef struct {
    int count;
    // Map<PackageName, KnownVersions>
    HashMap* packages;
} Registry;

typedef struct {
    Version newest;
    Version* previous;  // Array
    int previous_count;
} KnownVersions;
```

#### Registry Operations
- Binary serialization/deserialization for caching
- HTTP client for fetching updates
- Incremental updates via `/all-packages/since/N` endpoint

### Constraint System

#### Constraint Types
```c
typedef enum {
    EXACT,
    UNTIL_NEXT_MINOR,
    UNTIL_NEXT_MAJOR,
    ANY
} ConstraintType;

typedef struct {
    ConstraintType type;
    Version* exact_version;  // For EXACT
} Constraint;
```

#### Constraint Operations
- `constraint_satisfies`: Check if version meets constraint
- `constraint_intersect`: Merge two constraints
- Version comparison functions

### HTTP and Caching

#### Package Downloads
- HTTP client for downloading `elm.json` files
- SHA1 verification for archives
- Local file caching with directory structure

#### Cache Structure
```
$ELM_HOME/0.19.1/
├── packages/
│   ├── registry.dat
│   └── author/package/version/
│       ├── elm.json
│       └── src/
```

### Error Handling

#### Error Types
```c
typedef enum {
    NO_SOLUTION,
    NO_OFFLINE_SOLUTION,
    NETWORK_ERROR,
    INVALID_PACKAGE_DATA,
    CACHE_CORRUPTION
} SolverError;
```

#### Recovery Strategies
- Graceful degradation to offline mode
- Cache validation and repair
- Detailed error reporting

### Implementation Phases

1. **Phase 1: Core Data Structures**
   - Implement basic types (Version, PackageName, etc.)
   - Hash map and dynamic array utilities
   - Constraint system

2. **Phase 2: Registry Management**
   - Registry parsing and caching
   - HTTP client integration
   - Version retrieval functions

3. **Phase 3: Basic Solver**
   - Single strategy resolution
   - Goal exploration loop
   - Version addition logic

4. **Phase 4: Backtracking**
   - Multiple strategy support
   - State management for backtracking
   - `oneOf` implementation

5. **Phase 5: Integration**
   - HTTP downloading for constraints
   - Cache management
   - Error handling and reporting

6. **Phase 6: Optimization**
   - Constraint caching
   - Parallel downloading
   - Performance profiling

### Dependencies

#### Required C Libraries
- **HTTP Client**: libcurl for network operations
- **JSON Parsing**: cJSON or similar for elm.json parsing
- **Hash Maps**: uthash or implement custom
- **SHA1**: or similar for verification
- **File I/O**: Standard C file operations

#### Optional Optimizations
- Multi-threading for parallel downloads
- Memory pooling for frequent allocations
- Persistent caching improvements

### Testing Strategy

1. **Unit Tests**: Individual functions (constraint satisfaction, version comparison)
2. **Integration Tests**: Full resolution scenarios
3. **Regression Tests**: Known problematic dependency cases
4. **Performance Tests**: Large dependency trees
5. **Offline Tests**: Cached-only resolution

### Challenges and Considerations

#### Haskell to C Translation
- Monadic backtracking → explicit state management
- Lazy evaluation → eager computation
- Garbage collection → manual memory management
- Type safety → careful pointer handling

#### Performance Considerations
- Minimize allocations in hot paths
- Efficient data structures for lookups
- Parallel downloading where possible
- Cache hit optimization

#### Maintainability
- Clear separation of concerns
- Comprehensive error handling
- Extensive logging and debugging support
- Modular design for easy testing

This plan provides a comprehensive roadmap for re-implementing the Elm package solver in C, maintaining the core algorithm while adapting to C's imperative paradigm.

## PubGrub-Based Re-Implementation (Alternative / Recommended Evolution)

### Why Consider PubGrub?
PubGrub (used by Dart and adopted conceptually by SwiftPM) is a modern dependency resolution algorithm designed to produce precise, human-readable error messages while efficiently navigating complex version constraints. Compared to the current Elm solver:
- **Current Solver**: Depth-first backtracking (DFS), newest-first heuristic, minimal conflict explanations.
- **PubGrub**: Maintains a trail of decisions and derived incompatibilities, performs conflict-driven backjumping (not just linear backtracking), and constructs detailed root-cause explanations for unsatisfiable constraints.
- **Benefit for Elm**: Richer error diagnostics (e.g., “Package A requires B <2.0.0 but C requires B >=2.1.0”), fewer spurious retries, extensibility for future constraint types.

### Conceptual Mapping (Elm → PubGrub)
- **Package Identity**: `Pkg.Name` → symbol/node.
- **Version Domain**: Sorted list from registry (`KnownVersions`): newest-first still usable as a decision heuristic.
- **Constraints**: Elm’s `C.Constraint` (exact, until-next-minor, until-next-major, anything) → PubGrub “version ranges”. Convert:
    - `exactly v` → singleton range `[v, v]`.
    - `untilNextMinor v` (e.g. 1.2.x) → `[v, <1.3.0)`.
    - `untilNextMajor v` (e.g. 1.x) → `[v, <2.0.0)`.
    - `anything` → full range `[* , *]`.
- **Elm Version Compatibility (`_elm :: C.Constraint`)**: Additional filtered range applied before deriving dependencies; treat incompatibility as immediate derivation of a conflict clause.
- **Dependency Edges**: Each selected `(package, version)` introduces a set of constraints on its transitive dependencies → PubGrub “incompatibilities” formed from “package range” literals.

### PubGrub Core Concepts Recap
1. **Partial Assignment**: Ordered decisions choosing a version for a package.
2. **Incompatibilities**: Clauses describing combinations that cannot coexist (e.g. `A in [1.0.0]` and `B in [>=2.0.0]` cannot both hold).
3. **Propagation**: Narrow ranges for other packages until either a single version remains (forcing) or conflict arises.
4. **Conflict Resolution**: Analyze the incompatibility graph to produce a new learned incompatibility (backjump) rather than stepwise backtrack.
5. **Backjumping**: Jump directly to the highest decision level implicated in the conflict.
6. **Error Reporting**: Trace learned incompatibilities back to origins to explain the failing dependency chain.

### Differences vs Existing Elm Solver
| Aspect | Current Solver | PubGrub |
|--------|----------------|---------|
| Search | DFS newest-first | Decision levels + propagation |
| Conflict Handling | Simple backtrack | Clause-style learned incompatibilities |
| Error Messages | Limited (“No solution”) | Rich causal chains |
| Data Cached | Constraints per (pkg, version) | Same plus incompatibility graph |
| Performance | Adequate (small ecosystem) | Scales better with richer constraints |

### High-Level C Design for PubGrub Library
Core modules:
1. `version.h/.c` – SemVer parsing, comparison, range representation.
2. `range.h/.c` – Normalized disjoint intervals; intersection, subtraction.
3. `registry.h/.c` – Loading `registry.dat`, querying versions (already outlined).
4. `constraints.h/.c` – Translating Elm constraints → ranges; merging.
5. `incompatibility.h/.c` – Structs representing a clause (vector of terms) plus reason metadata.
6. `state.h/.c` – Decision stack, assignments, packages pending propagation.
7. `propagation.h/.c` – Implements range narrowing and forced version detection.
8. `resolution.h/.c` – Main solving loop (decide, propagate, resolve conflict, learn, backjump).
9. `explain.h/.c` – Human-readable failure explanation builder.
10. `cache.h/.c` – Persistent constraint + incompatibility caching (optional advanced feature).

### Key Data Structures (Sketch)
```c
typedef struct { char *name; } Package;

typedef struct { int major, minor, patch; } Version;

typedef struct { Version lower; bool lower_inclusive; Version upper; bool upper_inclusive; bool unbounded_lower; bool unbounded_upper; } VersionRange;

typedef struct { Package *pkg; VersionRange range; } Term; // Package must satisfy range

typedef enum { REASON_DEP_EDGE, REASON_ELM_COMPAT, REASON_NO_VERSIONS, REASON_USER_ROOT } IncompatibilityReason;

typedef struct { Term *terms; size_t term_count; IncompatibilityReason reason; struct Incompatibility **derived_from; size_t derived_count; } Incompatibility;

typedef struct { Package *pkg; VersionRange range; int decision_level; bool decided; } Assignment; // decided or derived

typedef struct { Assignment *items; size_t count; } AssignmentTrail;
```

### Algorithm Loop (Pseudo-C)
```c
while (!all_root_packages_satisfied()) {
        if (propagate(state, &conflict)) {
                if (!conflict) {
                        decide_next_package(state); // pick unsatisfied with heuristic (e.g. newest-first narrowing)
                } else {
                        Incompatibility *learned = analyze_conflict(state, conflict);
                        add_incompatibility(state, learned);
                        if (!backjump(state, learned)) return SOLVER_NO_SOLUTION;
                }
        }
}
return SOLVER_OK;
```

### Mapping Elm’s Strategy Layers to PubGrub
Elm currently retries with exact vs range constraints on direct dependencies. Under PubGrub:
- Encode all potential strategies as initial ranges simultaneously (e.g. start with widest acceptable ranges: until-next-minor/major/anything).
- Apply a heuristic scoring to prefer newest versions within range when making decisions (no need for full re-run loops).
- If you want to emulate original ordering strictly, you can pre-constrain roots to exact versions first, run, and relax if unsat—but PubGrub typically handles wide ranges gracefully.

### Conflict Explanation Example
If `A` depends on `B <2.0.0` and `C` depends on `B >=2.1.0`:
1. Decision: pick `A` version 1.0.0 ⇒ propagate `B <2.0.0`.
2. Decision: pick `C` version 3.0.0 ⇒ propagate `B >=2.1.0`.
3. Propagation finds empty intersection ⇒ conflict incompatibility `{ B <2.0.0, B >=2.1.0 }`.
4. Analyze traces: report root cause referencing chosen versions of `A` and `C`.

### Incremental & Offline Support
- If offline: only include versions present locally; `REASON_NO_VERSIONS` incompatibilities produced when range has no matching cached versions.
- Registry updates prior to solving; fallback on cached registry if network unavailable.

### Error Handling Enhancements
Add structured error types:
```c
typedef enum {
    SOLVER_OK,
    SOLVER_NO_SOLUTION,
    SOLVER_OFFLINE_INCOMPLETE,
    SOLVER_BAD_CACHE,
    SOLVER_IO_ERROR
} SolverStatus;
```
`SOLVER_NO_SOLUTION` accompanied by an explanation tree assembled from incompatibility derivations.

### Implementation Phases (PubGrub Variant)
1. Core version & range utilities.
2. Registry + version domain enumeration.
3. Term/Incompatibility structures + basic formatting.
4. Assignment trail & decision management.
5. Propagation engine (range intersection & forced assignments).
6. Conflict analysis + backjumping (construct learned incompatibility).
7. Integration with constraint loading (`elm.json`).
8. Explanation rendering.
9. Optimization: heuristic tuning, caching learned incompatibilities across runs.
10. Offline mode refinements & test matrix.

### Testing Focus for PubGrub
- Artificial conflicting dependency graphs for backjump depth correctness.
- Stress tests: wide version ranges with sparse actual versions.
- Regression: replicate scenarios solved by current solver; ensure identical or better solutions.
- Explanation fidelity: snapshot textual output for golden tests.
