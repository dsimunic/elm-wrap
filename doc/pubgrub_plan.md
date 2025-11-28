# PubGrub Implementation of Elm’s Dependency Solver in C

This document lays out a concrete design and implementation plan for replacing the current stub solver in `wrap/src/solver.c` with a full [PubGrub](https://github.com/dart-lang/pub/blob/master/doc/solver.md) implementation, tailored to Elm’s package ecosystem and integrated into the existing `wrap` C codebase.

It is based on:

- Dart’s reference PubGrub spec: `dart-lang/pub/doc/solver.md`.
- Natalie Weizenbaum’s article “PubGrub: Next-Generation Version Solving”.
- The Elm `elm-pubgrub` library (`mpizenberg/elm-pubgrub`) and its Elm‑package specific example.
- Existing wrap docs and scaffolding: `doc/solver.md`, `doc/package_downloading.md`, `doc/install_implementation.md`, `doc/implementation_minizinc.md`, and the current `src/solver.c/.h`, `src/install.c`, `src/cache.c`, `src/elm_json.c`.

The goal is a production‑quality, synchronous C implementation that:

- Matches Elm’s semantics (strategies, offline behavior, registry layout).
- Uses PubGrub’s conflict‑driven clause learning for performance.
- Produces human‑readable error messages similar to Pub’s.
- Fits within the current `wrap` architecture (cache, elm.json, install).

---

## 1. High-Level Architecture

### 1.1 Overall Shape

We will structure the solver as a small C library inside `wrap/src`:

- `solver.h/.c`
  - Public API used by `install.c` and future commands.
  - Thin wrapper around the PubGrub engine and Elm‑specific wiring.
- `pubgrub_core.h/.c`
  - PubGrub algorithm and core data structures (terms, incompatibilities, assignments, decision levels, propagation).
  - Generic over “package id”, “version”, and “dependency provider”.
- `pubgrub_elm.h/.c`
  - Adapts Elm’s registry and package metadata (via `cache.c` and filesystem) to PubGrub’s generic interface.
  - Encodes Elm version compatibility and Elm’s constraint ladder (`exact`, `untilNextMinor`, `untilNextMajor`, `any`).

We keep a clear separation:

- **Core**: algorithm, conflict resolution, backjumping, explanation tree.
- **Elm adapter**: how to enumerate versions and dependencies, mapping Elm constraints to version ranges.
- **CLI integration**: how to call the solver when the user runs `wrap install`.

### 1.2 Execution Model

- **Synchronous**: the solver runs to completion on a single thread, performing filesystem and network I/O as needed through the cache/registry layer.
- **Single-shot**: each call solves a single root project state (current `elm.json` + requested changes) into a complete dependency assignment.
- **Strategies as separate runs**: Elm’s “strategy ladder” (exact vs range constraints on direct deps) is implemented outside PubGrub as multiple calls to the solver with different root constraints.

We will not implement the `elm-pubgrub` stepwise/async API initially, but we keep the design compatible with a future “resume/step” API if needed.

---

## 2. Mapping Elm’s Model to PubGrub Concepts

PubGrub (per `dart-lang/pub/doc/solver.md`) is defined in terms of:

- **Terms**: statements about a single package’s acceptable versions, positive or negative (e.g. `foo ^1.0.0`, `not foo <2.0.0`).
- **Incompatibilities**: sets of terms that cannot all be satisfied together.
- **Partial solution / assignments**: ordered list of decisions and derivations, each with a decision level and optional cause incompatibility.
- **Derivation graph**: explains why each incompatibility holds, used to build good error messages.

We map Elm concepts onto that:

- **Package identity**
  - Elm: `author/name` (`Pkg.Name`).
  - C: `ElmPackageId { char *author; char *name; }`, with canonicalization and a stable integer ID for efficient maps.

- **Version**
  - Elm: semantic version (`major.minor.patch`), sorted newest first in registry.
  - C: `Version { int major, minor, patch; }`, plus string representation for I/O.

- **Version ranges / constraints**
  - PubGrub: arbitrary ranges; internally normalized to conjunctions of `[lower, upper)` intervals.
  - Elm: `exactly v`, `untilNextMinor v`, `untilNextMajor v`, `any`.
  - Mapping:
    - `exact v` → `VersionRange [v, v]`.
    - `untilNextMinor v = a.b.c` → `[a.b.c, a.(b+1).0)`.
    - `untilNextMajor v = a.b.c` → `[a.b.c, (a+1).0.0)`.
    - `any` → `(-∞, +∞)` for that package (unbounded range).

- **Root package**
  - Elm: the project described by `elm.json` (application or package).
  - PubGrub: a distinguished `root` package with a single synthetic version; its dependencies encode the project’s direct constraints.
  - C: we construct a synthetic `root` package (`author="__root__"`, `name="__project__"`, version `1.0.0`) whose dependencies match current `elm.json` plus the requested install target.

- **Elm version compatibility**
  - Each package version has an `elm` constraint. We treat “incompatible Elm version” as an incompatibility like Pub’s SDK constraint:
    - For a package `p`, let `ElmCompatRange_p` be the range of Elm versions it supports. If the current compiler’s Elm version is not in this range, we represent this as an incompatibility forbidding that `(p, incompatible range)` selection.
  - In practice, we will **filter out** incompatible package versions when enumerating available versions.

- **Dependency edges**
  - For each concrete `(pkg, version)` we derive dependencies from its `elm.json` and build incompatibilities of the form:
    - “If we select `(pkg, version)` then we must also pick a version of `dep` inside a certain range”.
    - PubGrub form: `{ pkg ∈ [v_lo, v_hi), not dep ∈ AllowedRange }`.

---

## 3. C Data Structures

### 3.1 Version and Ranges

New core types (likely in `pubgrub_core.h`):

```c
typedef struct {
    int major;
    int minor;
    int patch;
} PgVersion;

typedef struct {
    PgVersion v;
    bool inclusive;
    bool unbounded;  // if true, ignore v/inclusive
} PgBound;

typedef struct {
    PgBound lower;  // lower.unbounded == true → no lower bound
    PgBound upper;  // upper.unbounded == true → no upper bound
    bool is_empty;  // true if range is empty after normalization
} PgVersionRange;
```

Helper operations:

- `pg_version_cmp(PgVersion a, PgVersion b)`
- `pg_range_intersect(PgVersionRange a, PgVersionRange b)`
- `pg_range_contains(PgVersionRange r, PgVersion v)`
- Constructors: `pg_range_any()`, `pg_range_exact(v)`, `pg_range_until_next_minor(v)`, etc.

### 3.2 Packages and IDs

We keep a thin struct for the public API, and map it to integer IDs internally:

```c
typedef struct {
    char *author;
    char *name;
} ElmPackageId;

typedef int PgPackageId; // internal 0..N-1
```

We maintain a bidirectional map:

- `PgPackageId` → `ElmPackageId` (array indexed by ID).
- `(author, name)` → `PgPackageId` (hash table or search in small array).

### 3.3 Terms and Incompatibilities

```c
typedef struct {
    PgPackageId pkg;
    PgVersionRange range;
    bool positive;  // true: pkg in range; false: not (pkg in range)
} PgTerm;

typedef enum {
    PG_REASON_DEPENDENCY,
    PG_REASON_NO_VERSIONS,
    PG_REASON_ROOT_DEP,     // root package constraint
    PG_REASON_USER_REQUEST, // explicit requested package
    PG_REASON_INTERNAL      // derived during resolution
} PgIncompatibilityReason;

typedef struct PgIncompatibility {
    PgTerm *terms;
    size_t term_count;
    PgIncompatibilityReason reason;

    // For error explanations:
    struct PgIncompatibility **causes;
    size_t cause_count;
} PgIncompatibility;
```

Incompatibilities are normalized:

- At most one term per package (per Pub’s spec).
- Derived incompatibilities remove root‑positive terms where possible.

### 3.4 Assignments and Partial Solution

```c
typedef struct {
    PgPackageId pkg;
    PgVersionRange range;
    bool positive;

    bool decided;                // decision or derivation
    int decision_level;          // 0 = before root, 1 = root, ≥2 further decisions
    PgIncompatibility *cause;    // NULL for decisions
} PgAssignment;

typedef struct {
    PgAssignment *items;
    size_t count;
    size_t capacity;
} PgAssignmentTrail;
```

Utility views:

- For each package, we can query:
  - The combined **positive** range (intersection of all positive assignments).
  - Whether there is a decision already for that package.
  - Any negative assignments (for conflict reasoning).

We’ll maintain:

- `PgAssignmentTrail trail;`
- A per‑package index summarizing current state (e.g. latest assignment index, current positive range) to avoid O(N) scans on every query.

### 3.5 Solver State and Dependency Provider

We define a generic dependency provider interface:

```c
typedef struct {
    // Enumerate all known versions for a package, newest first.
    // Returns number of versions, or -1 on error.
    int (*get_versions)(
        void *ctx,
        PgPackageId pkg,
        PgVersion *out_versions,
        size_t out_capacity
    );

    // Load dependencies for a concrete package version.
    // Fills an array of (dep package id, allowed range).
    int (*get_dependencies)(
        void *ctx,
        PgPackageId pkg,
        PgVersion version,
        PgPackageId *out_pkgs,
        PgVersionRange *out_ranges,
        size_t out_capacity
    );
} PgDependencyProvider;
```

Elm‑specific implementation (`pubgrub_elm.c`) will:

- Use `CacheConfig` / `cache.c` utilities to:
  - Ensure the registry is available (`registry.dat` or JSON).
  - Enumerate versions and their ordering.
  - Load each package’s `elm.json` to derive dependencies and Elm compatibility.
- Cache loaded dependency info in memory to avoid repeated disk reads.

Core solver state:

```c
typedef struct {
    PgDependencyProvider provider;
    void *provider_ctx;

    PgAssignmentTrail trail;
    PgIncompatibility **incompatibilities;
    size_t incompatibility_count;

    // Temporary buffers for propagation and conflict resolution:
    PgPackageId *changed_pkgs;
    size_t changed_count;

    // Root package id and its synthetic version.
    PgPackageId root_pkg;
    PgVersion   root_version;
} PgSolver;
```

---

## 4. PubGrub Algorithm in C

This section adapts the algorithm from `dart-lang/pub/doc/solver.md` into a concrete C‑oriented plan.

### 4.1 Main Solving Loop

Pseudo‑code for the core loop:

```c
PgSolverStatus pg_solve(PgSolver *s) {
    PgPackageId next = s->root_pkg;

    for (;;) {
        PgIncompatibility *conflict = NULL;

        if (!pg_unit_propagate(s, next, &conflict)) {
            return PG_SOLVER_INTERNAL_ERROR;
        }

        if (conflict != NULL) {
            PgIncompatibility *root_cause =
                pg_resolve_conflict(s, conflict);

            if (root_cause == NULL) {
                // Unsatisfiable: build explanation from root_cause.
                return PG_SOLVER_NO_SOLUTION;
            }

            // Conflict resolution backtracks trail as needed and returns
            // an incompatibility to be recorded.
            pg_add_incompatibility(s, root_cause);

            // Continue with next package at the new decision level.
            next = pg_pick_next_package(s);
            if (next < 0) return PG_SOLVER_OK; // All satisfied.
            continue;
        }

        // No more derivations at current state → make new decision.
        PgDecision decision;
        if (!pg_make_decision(s, &decision)) {
            // Either solved or no valid decisions remain.
            return decision.status; // e.g. PG_SOLVER_OK or PG_SOLVER_NO_SOLUTION
        }

        next = decision.next_pkg;
    }
}
```

Key functions to implement:

- `pg_unit_propagate(PgSolver*, PgPackageId next, PgIncompatibility **out_conflict)`
  - Follows the spec’s “Unit Propagation” section.
  - Maintains `changed_pkgs` queue, scans relevant incompatibilities for “almost satisfied” ones, adds derivations, detects satisfied incompatibilities as conflicts.
- `pg_resolve_conflict(PgSolver*, PgIncompatibility *conflict)`
  - Follows the spec’s “Conflict Resolution” section.
  - Walks back through the trail using causes, applies generalized resolution to derive a new incompatibility, decides the backjump level (previous satisfier).
  - Returns `NULL` when the root package itself is unsatisfiable.
- `pg_make_decision(PgSolver*, PgDecision *out)`
  - Implements “Decision Making” and version picking heuristic:
    - Choose a package with a positive derivation but no decision.
    - Compute outstanding range for that package by intersecting all relevant assignments.
    - Use provider to list versions in that range.
    - If there are no versions left → add incompatibility `{term}` and treat it as immediate conflict.
    - Otherwise choose a version (see heuristic below), push decision onto trail, and lazily create incompatibilities for its dependencies.
- `pg_pick_next_package` is effectively handled by `pg_make_decision` (it chooses the package).

### 4.2 Version Picking Heuristic

Following Dart’s implementation:

- For each candidate package:
  - Compute the number of matching versions (domain size).
  - Prefer packages with *fewer* matching versions.
- For the chosen package:
  - Select the latest version within the allowed range.

Simplifications for first implementation:

- Initially, we can just:
  - Pick the first package in some stable order with a positive derivation but no decision.
  - Use provider’s “newest first” ordering, choose the first version in range.
- Once basic correctness is established, refine heuristic toward Dart’s behavior.

### 4.3 Dependency Conversion (Lazy Incompatibilities)

Per Pub’s spec:

- When we first decide a concrete version `(pkg, v)`:
  - We fetch its dependencies using `provider.get_dependencies`.
  - For each dependency `(dep_pkg, dep_range)` we create one or more incompatibilities.
- We **collapse** identical dependency constraints across adjacent versions:
  - Example: if `foo 1.0.0` and `foo 1.1.0` both depend on `bar ^1.0.0`, we create a single incompatibility:
    - `{ foo ∈ [1.0.0, 1.2.0), not bar ∈ ^1.0.0 }` where `1.2.0` is the first version without that dependency.
  - This is an optimization; the first iteration may create per‑version incompatibilities and later refactor to collapse them by scanning neighboring versions.

Special cases:

- If an `(pkg, v)` is unusable (e.g. incompatible Elm version, missing metadata), instead of adding its dependencies we add an incompatibility forbidding that version (and possibly adjacent unusable versions).

---

## 5. Elm-Specific Wiring

### 5.1 Registry and Versions

Using `cache.c` and `package_downloading.md`:

- Load or update the registry (`registry.dat` or `all-packages.json`).
- For each package:
  - Build a `KnownVersions` record (newest version + list of previous).
  - Map those to `PgVersion` values, sorted newest first.
- Expose this via `PgDependencyProvider.get_versions`:
  - `ctx` carries a pointer to a `Registry` structure and `CacheConfig`.
  - The provider implementation filters out versions incompatible with the compiler’s Elm version.

### 5.2 Dependencies and `elm.json`

For `PgDependencyProvider.get_dependencies`:

- Use `cache_package_exists` / `cache_download_package` to ensure the package’s folder and `elm.json` are present.
- Use `elm_json_read` to parse the package’s `elm.json`.
- For each dependency entry:
  - Convert its Elm constraint (`exact`, `upperBound`, etc.) to a `PgVersionRange`.
  - Add `(dep_pkg_id, dep_range)` to the output arrays.
- Also enforce **Elm version compatibility**:
  - If the package’s `elm` constraint does not include the compiler’s Elm version, treat `(pkg, v)` as unsatisfiable.
  - Either omit it from `get_versions` or add a “never select this version” incompatibility.

### 5.3 Root Package Construction

When we call `solver_add_package` from `install.c`:

- Build a synthetic `root` package in memory:
  - `PgPackageId root = 0` (reserved).
  - A single synthetic version `1.0.0`.
- From `ElmJson`:
  - Collect current direct dependencies (and test dependencies if `--test`).
  - Add the requested new package as an additional dependency.
  - Encode constraints according to Elm’s strategy ladder (see below).
- Provide these dependencies via a special case in `get_dependencies`:
  - If `pkg == root_pkg`, return the root dependency list without touching cache/network.

### 5.4 Strategy Ladder (`addToApp` Semantics)

Elm’s solver tries multiple strategies in order (simplified here to the application case):

1. **Exact versions for all dependencies** (direct + indirect).
2. **Exact versions for direct dependencies only**.
3. **Until next minor** for direct dependencies.
4. **Until next major** for direct dependencies.
5. **Any version** for direct dependencies.

Plan:

- Implement a small wrapper in `solver.c`:
  - Given current `ElmJson` and requested `(author, name)`, generate a root dependency list for each strategy.
  - For each strategy:
    1. Construct root dependencies.
    2. Initialize `PgSolver` with the same provider (registry + deps).
    3. Run `pg_solve`.
    4. If it returns `PG_SOLVER_OK`, apply the solution to `ElmJson` and stop.
    5. If it returns `PG_SOLVER_NO_SOLUTION`, continue to the next strategy.
- If all strategies fail, surface `SOLVER_NO_SOLUTION` with the explanation from the last attempt (which should already mention the relevant constraints).

### 5.5 Updating `elm.json` and Cache

On successful solve:

- Extract the chosen versions from the `PgAssignmentTrail`:
  - For each `PgPackageId` except `root_pkg`, find its final **positive** range.
  - Because the algorithm ensures only a single concrete version, that range collapses to a singleton.
- Convert back to `(author, name, version string)` using the package/registry mappings.
- For application projects:
  - Update `dependencies_direct`, `dependencies_indirect`, `dependencies_test_direct`, `dependencies_test_indirect` according to promotion rules (existing logic in `elm_json.c`).
- For package projects:
  - Update `package_dependencies` / `package_test_dependencies`.
- Ensure all chosen packages are downloaded:
  - Use `cache_package_exists` and `cache_download_package` for any missing archives.

---

## 6. Error Reporting and Explanations

PubGrub’s strength is clear explanations via derivation graphs (see the “Error Reporting” and “Examples” sections in `pub/doc/solver.md`).

### 6.1 Storing Derivation Graph

We already keep:

- For each derived assignment: a `PgIncompatibility *cause`.
- For each derived incompatibility: `causes[]` pointing to its predecessors.

This matches Dart’s derivation graph.

### 6.2 Building Text Explanations

We will implement a dedicated formatter in `pubgrub_elm.c`:

- Input:
  - Root incompatibility that proves `{root any}` is unsatisfiable.
- Output:
  - Multi‑line explanation similar to the examples in the PubGrub article and in `elm-pubgrub`:
    - “Because A >=1.0.0 depends on B <2.0.0 and C depends on B >=2.1.0, every version of A requires B <2.0.0, …”.

Implementation plan:

1. Follow the approach from Dart and Elm `elm-pubgrub`:
   - Recursively walk the derivation graph from the root incompatibility.
   - For each internal node, generate a sentence citing its causes and the packages/constraints involved.
2. Reuse or mirror some of the textual patterns from `elm-pubgrub` where appropriate.
3. Emit explanations through `stderr` in `solver_add_package` when `SOLVER_NO_SOLUTION` is returned.

We keep the explanation generator logically separate from the core algorithm so it can be improved independently.

---

## 7. Integration with Existing `wrap` Code

### 7.1 Public Solver API (`solver.h`)

We keep the existing public API shape, but change implementation and add a couple of helper types:

- `SolverState *solver_init(CacheConfig *cache, bool online);`
  - Now owns:
    - `CacheConfig *cache`.
    - `Registry` handle or lazily initialized registry object.
    - Compiler Elm version (if needed).
    - Settings for logging/verbosity.
- `SolverResult solver_add_package(SolverState*, ElmJson*, const char *author, const char *name, bool is_test_dependency);`
  - Implements:
    1. Registry availability checks and offline error handling (as already stubbed).
    2. Strategy ladder loop (Section 5.4).
    3. PubGrub solving for each strategy.
    4. On success: update `ElmJson` and download any missing packages.
    5. On failure: print human‑readable explanation before returning `SOLVER_NO_SOLUTION`.

We may extend `SolverResult` with:

- `SOLVER_BAD_CACHE`
- `SOLVER_INTERNAL_ERROR`

and map them to user‑friendly messages in `install.c`.

Existing constraint helper functions (`Constraint`, `version_satisfies`, …) can either:

- Be used to construct `PgVersionRange` instances, or
- Be replaced by more general range helpers, while keeping the public API unchanged for now.

### 7.2 CLI (`install.c`)

Minimal changes:

- `install_package` remains the entry point: it creates `SolverState`, calls `solver_add_package`, and handles `SolverResult`.
- Improve error messages to show the explanation string on `SOLVER_NO_SOLUTION`.
- No changes required for `install_all_dependencies` initially (it simply verifies/downloads packages based on existing `elm.json`).

### 7.3 Cache and Elm JSON

No interface changes to:

- `cache_config_init`, `cache_package_exists`.
- `elm_json_read`, `elm_json_write`, `elm_json_find_package`, `elm_json_promote_package`, `package_map_add`.

We only add:

- Functions to enumerate available versions for a package from the registry.
- Functions to read a package’s `elm.json` from the cache path and convert dependencies to `PgVersionRange`.

---

## 8. Implementation Phases

This is a suggested iteration plan to keep risk low and progress demonstrable.

### Phase 1 – Core Infrastructure

- Implement `PgVersion`, `PgVersionRange`, comparison and range operations.
- Add a small unit‑testable module (standalone C test or minimal harness) for:
  - Parsing version strings to `PgVersion`.
  - Converting Elm constraints to `PgVersionRange`.
  - Range intersection and emptiness detection.

### Phase 2 – PubGrub Core Engine

- Implement:
  - `PgTerm`, `PgIncompatibility`, `PgAssignment`, `PgAssignmentTrail`.
  - `PgSolver` structure.
  - Unit propagation (`pg_unit_propagate`) per spec.
  - Conflict resolution (`pg_resolve_conflict`) including generalized resolution and backjumping.
  - Decision making (`pg_make_decision`) with simple heuristics.
- Initially, use a trivial in‑memory dependency provider (for tests) that:
  - Has a small hard‑coded graph of packages.
  - Allows verifying correctness without filesystem/network.

### Phase 3 – Elm Dependency Provider

- Implement `pubgrub_elm.c` provider:
  - Wrap `CacheConfig`, `registry.dat` parsing (or JSON for initial prototype).
  - Enumerate versions in newest‑first order.
  - Load package `elm.json` from cache, parse dependencies, convert to `PgVersionRange`.
- Implement simple caching of parsed `elm.json` files in memory.
- Add an integration test harness (even if manual) that:
  - Builds a fake registry and a few cache folders on disk.
  - Runs the solver and prints results.

### Phase 4 – Strategy Ladder and `solver_add_package`

- Implement the 5‑step strategy ladder in `solver.c` using the Elm docs and `builder/src/Deps/Solver.hs` as reference.
- Implement mapping from PubGrub’s final assignment to `ElmJson`:
  - Update direct and indirect dependency maps consistently.
  - Reuse existing promotion logic.
- Wire this into `install.c` and verify the user workflow for:
  - Installing a new package.
  - Re‑installing an existing package (promotion only, solver not needed).

### Phase 5 – Error Explanations

- Build the derivation‑graph‑based explanation generator:
  - Start with a simple, linear explanation (akin to the “Because X depends on Y…” examples).
  - Add support for branching explanations as in PubGrub’s “Branching Error Reporting” section.
- Integrate with `install.c` to show explanations on `SOLVER_NO_SOLUTION`.
- Add tests using fixed registries where resolution is intentionally impossible.

### Phase 6 – Performance and Robustness

- Optimize:
  - Package and version lookup (interning IDs, hash maps).
  - Incompatibility indexing so propagation only scans relevant ones per changed package.
  - Version picking heuristics to minimize backtracking.
- Harden:
  - Handling of corrupt cache or partial registry.
  - Offline mode behavior: fail early with `SOLVER_NO_OFFLINE_SOLUTION` when required data is missing.
  - Memory management and leak checks.

---

## 9. Testing Strategy

- **Unit tests** for:
  - Version parsing and range logic.
  - Constraint/range intersection.
  - Core PubGrub routines (propagation, conflict resolution, backjumping) using a tiny in‑memory dependency graph.
- **Integration tests** for:
  - Solving real‑world‑style Elm dependency graphs (mirroring examples from `elm-pubgrub` and the Dart spec).
  - Satisfiable and unsatisfiable scenarios, checking both the chosen versions and the shape of explanations.
- **CLI‑level tests** (manual or scripted) for:
  - `wrap install` with various combinations of new and existing packages.
  - Offline vs online behavior.

The first target is **correctness** (matching Elm’s expected solutions and error cases), then **explanation quality**, then **performance**.

---

## 10. Summary

- We will implement PubGrub as a dedicated core in C, closely following Dart’s formal description and inspired by `elm-pubgrub`.
- Elm‑specific behavior (registry, Elm version compatibility, strategy ladder, install/update semantics) is layered on top via a dependency provider and the existing `cache` and `elm_json` modules.
- The final `solver_add_package` API becomes a robust entry point for `wrap install`, providing both high‑quality solutions and detailed explanations when no solution exists.

