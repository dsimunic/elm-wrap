# PubGrub Implementation Notes (Current State & Next Steps)

This document is a working implementation guide that complements the higher‑level design in `wrap/doc/pubgrub_plan.md`. It focuses on:

- What is already implemented in `wrap/src/pgsolver` and `wrap/src/solver.c`.
- What is still missing to reach a full PubGrub implementation.
- Concrete coding tasks and test ideas so you can pick up work later without re‑deriving the plan.

For architectural background and terminology, refer to `wrap/doc/pubgrub_plan.md`.

---

## 1. Current Implementation Snapshot

### 1.1 Core Types and Utilities (`pg_core.*`)

Implemented according to Section 3 of `pubgrub_plan.md`:

- `PgVersion`
  - Parsed from `"X.Y.Z"` via `pg_version_parse`.
  - Compared via `pg_version_compare`.
- `PgVersionRange`
  - Represented as `(lower, upper)` bounds (`PgBound`), each possibly unbounded and inclusive/exclusive.
  - Constructors:
    - `pg_range_any`
    - `pg_range_exact`
    - `pg_range_until_next_minor`
    - `pg_range_until_next_major`
  - Operations:
    - `pg_range_intersect`
    - `pg_range_contains`
- `PgDependencyProvider`
  - Interface matches `pubgrub_plan.md`:
    - `get_versions(ctx, pkg, out_versions, out_capacity)`
    - `get_dependencies(ctx, pkg, version, out_pkgs, out_ranges, out_capacity)`

Internal (file‑local) core structures:

- `PgTerm`, `PgIncompatibility`, `PgAssignment`, `PgAssignmentTrail`
  - Defined and used to record:
    - Root dependency constraints as positive assignments at decision level 0.
    - Chosen versions (decisions) and their propagated dependency constraints as additional assignments on the trail.
  - Still **not yet used** for full PubGrub behavior (no unit propagation over incompatibilities, no conflict learning/backjumping yet).
- `PgPkgState`
  - Maintains per‑package solver state:
    - `used`
    - `has_required`, `required` (`PgVersionRange`) as a summary of all positive assignments for that package.
    - `fixed`, `fixed_version` (`PgVersion`)
  - Stored as `PgPkgState *pkg_states` in `PgSolver`. Over time this is intended to become only a summary/index over the assignment trail.

Public solver API:

- `PgSolver *pg_solver_new(PgDependencyProvider provider, void *provider_ctx, PgPackageId root_pkg, PgVersion root_version);`
- `void pg_solver_free(PgSolver *solver);`
- `bool pg_solver_add_root_dependency(PgSolver *solver, PgPackageId pkg, PgVersionRange range);`
- `PgSolverStatus pg_solver_solve(PgSolver *solver);`
- `bool pg_solver_get_selected_version(PgSolver *solver, PgPackageId pkg, PgVersion *out_version);`

### 1.2 Current Solver Logic (Full PubGrub Implementation)

The current `pg_solver_solve` implements the complete PubGrub algorithm as described in `pubgrub_plan.md`:

1. Seeds the assignment trail with an exact decision for the root package (`root_pkg` / `root_version`).
2. Registers root dependencies as incompatibilities.
3. Main loop:
   - Performs unit propagation over incompatibilities to derive new assignments or detect conflicts.
   - If a conflict is found, resolves it using generalized resolution and backjumps to an appropriate level, learning a new incompatibility.
   - If no conflict, makes a decision by choosing the most constrained undecided package and selecting its newest allowable version.
   - Registers dependency incompatibilities for the chosen version.
4. Stops when no more decisions can be made without conflicts.

This implements full CDCL-style PubGrub with conflict-driven backjumping and learned incompatibilities.

### 1.3 Elm Adapter (`pg_elm.*`)

Following Section 5 of `pubgrub_plan.md`, with a few simplifications:

- `PgElmContext`
  - Holds:
    - `CacheConfig *cache`
    - `bool online`
    - `(author, name) ↔ PgPackageId` mapping via:
      - `int package_count`, `int package_capacity`
      - `char **authors`, `char **names`
  - ID `0` is reserved for the synthetic root (`"__root__/__root__"`).

- Context management:
  - `pg_elm_context_new(cache, online)`:
    - Allocates and initializes the context and mapping arrays.
  - `pg_elm_context_free(ctx)`:
    - Frees all interned strings and arrays.

- Package interning:
  - `pg_elm_root_package_id()`:
    - Returns `0`.
  - `pg_elm_intern_package(ctx, author, name)`:
    - Reuses an existing `(author, name)` if present, otherwise assigns the next `PgPackageId`.

- Version provider (`get_versions`):
  - `pg_elm_provider_get_versions(ctx, pkg, out_versions, out_capacity)`:
    - For root pkg: returns a synthetic `1.0.0`.
    - For real packages:
      - Uses the cached registry data in `ctx->registry` (loaded from `registry.dat`).
      - Looks up `"author/name"` and reads the version array.
      - Parses versions via `pg_version_parse`.
      - Returns up to `out_capacity` versions, **newest‑first** (registry stores them oldest‑first, so reverses order).

- Dependency provider (`get_dependencies`):
  - `pg_elm_provider_get_dependencies(ctx, pkg, version, out_pkgs, out_ranges, out_capacity)`:
    - Ignores root package (root dependencies modeled separately).
    - For other packages:
      - Uses `cache_get_package_path` and appends `/elm.json`.
      - Reads `elm.json` via `elm_json_read`; if missing and `online`, calls `install_env_download_package` (or `cache_download_package_with_env`) and retries.
      - For Elm packages (`ELM_PROJECT_PACKAGE`) with `package_dependencies`:
        - For each dependency:
          - Parses constraint strings like `"1.0.0 <= v < 2.0.0"` into `PgVersionRange` using `pg_elm_parse_constraint_range`.
          - Interns the dependency `(author, name)` into a `PgPackageId`.
          - Populates `out_pkgs[i]` and `out_ranges[i]`.
      - Test dependencies and app projects are currently not modeled (only package dependencies).

- Provider construction:
  - `pg_elm_make_provider(ctx)` returns a `PgDependencyProvider` using the above callbacks.

### 1.4 CLI Integration (`solver.c`)

`solver_add_package` now uses the PubGrub core instead of the old stub:

- Preconditions:
  - Checks `state`, `elm_json`, `author`, `name`.
  - Ensures the registry is present (or downloads it) via `cache_registry_exists` / `cache_download_registry`.

- PubGrub setup:
  - Creates `PgElmContext *pg_ctx = pg_elm_context_new(state->cache, state->online)`.
  - Builds `PgDependencyProvider provider = pg_elm_make_provider(pg_ctx)`.
  - Gets `root_pkg = pg_elm_root_package_id();` and a synthetic `root_version = {1,0,0}`.
  - Constructs `PgSolver *pg_solver = pg_solver_new(provider, pg_ctx, root_pkg, root_version)`.

- Root requirements (application projects):
  - For each existing direct dependency in `elm_json->dependencies_direct`:
    - Interns `(author, name)` as `PgPackageId`.
    - Parses its exact version `X.Y.Z` into `PgVersion`.
    - Calls `pg_solver_add_root_dependency(pg_solver, pkg_id, pg_range_exact(v))`.
  - For each test direct dependency in `dependencies_test_direct`:
    - Same as above (currently treated as hard constraints; no separate test graph).

- Requested package requirement:
  - Interns `(author, name)` as `new_pkg_id`.
  - Adds an unconstrained requirement:
    - `pg_solver_add_root_dependency(pg_solver, new_pkg_id, pg_range_any())`.

- Solve and apply:
  - Runs `pg_solver_solve(pg_solver)`.
  - Checks `pg_solver_get_selected_version(pg_solver, new_pkg_id, &chosen)`.
  - Formats `chosen` into `"major.minor.patch"`.
  - Ensures that package is cached via `cache_package_exists` / `install_env_download_package`.
  - Adds it to the appropriate dependency map:
    - For apps: `dependencies_direct` or `dependencies_test_direct` depending on `is_test_dependency`.
    - For packages: `package_dependencies` or `package_test_dependencies` (currently treated as exact versions, not ranges).

This gives us a **working vertical slice**: registry → provider → solver → new package selection → cache and `elm.json` update.

---

## 2. Gaps vs `pubgrub_plan.md` (Core Algorithm)

Most of the core PubGrub algorithm has been implemented. The remaining gaps are minor:

1. **Real PubGrub loop** ✅
   - Implemented as described.

2. **Unit propagation over incompatibilities** ✅
   - Implemented with changed package queue for efficiency.

3. **Conflict resolution and backjumping** ✅
   - Implemented using generalized resolution.

4. **Decision making** ✅
   - Implemented with most-constrained package selection.

5. **Error explanations** ✅
   - Core error explanation function exists (`pg_solver_explain_failure`), but not yet wired into CLI.

6. **Performance improvements**
   - Current:
     - Simple arrays, O(N * M) scans in several places.
     - No indexing of incompatibilities by package.
   - Later:
     - Index incompatibilities per package to speed unit propagation.
     - Caching of version lists and dependency info in memory.

---

## 3. Gaps vs `pubgrub_plan.md` (Elm Adapter & Integration)

1. **Registry parsing and caching** ✅
   - The registry binary file (`registry.dat`) is loaded once per `PgElmContext` and cached in `ctx->registry`; each `get_versions` call reuses this parsed representation.

2. **Elm version compatibility**
   - Currently ignored.
   - Desired:
     - Filter versions by Elm compatibility (based on `elm.json` or registry metadata), matching the behavior described in `pubgrub_plan.md`.
     - Represent “incompatible with this Elm version” as incompatibilities or as filtered out versions.

3. **Application projects’ dependency graph**
   - Currently:
     - Only existing direct and test direct dependencies are modeled as root constraints, and only the newly requested package’s transitive deps are explored.
   - Desired:
     - For apps, model full transitive dependencies (direct, indirect, test-direct, test-indirect) as in the original Elm solver and `pubgrub_plan.md`.
     - After solving, split chosen versions into direct vs indirect maps correctly.

4. **Package projects’ constraints**
   - Currently:
     - `solver_add_package` treats package dependencies like exact versions when updating `elm.json`.
   - Desired:
     - For Elm package projects, preserve constraint ranges, not just a single exact version.
     - Root constraints for package projects should be ranges, not just `any` or exact.

5. **Strategy ladder (`addToApp`)** ✅
   - Implemented in `solver_add_package` with 3 strategies for normal adds, 1 for major upgrades.

6. **Test dependencies and test graph**
   - Currently:
     - Test dependencies are added as hard root constraints but not otherwise distinguished.
   - Desired:
     - Mirror Elm’s semantics for test dependencies (e.g. how test indirect deps are derived and reported).

---

## 4. Concrete Next Coding Steps

Recommended order (each bullet can be its own branch/PR):

1. **Introduce a small in‑memory provider for unit tests** ✅
   - Implemented in `wrap/src/pgsolver/pg_core_test.c` with a dedicated `make pg_core_test` target.
   - Provides a hard‑coded dependency graph plus a basic/conflict scenario so `pg_core` can be tested without Elm I/O.
   - Running `./bin/pg_core_test` exercises the current propagation loop and will serve as a harness for future PubGrub logic.

2. **Refactor solver state to rely on assignments instead of `PgPkgState`** ✅
   - `pg_core.c` now treats assignments as the source of truth: package ranges and decisions are derived by scanning/intersecting the trail.
   - `PgPkgState` keeps only summary bits (`used`, `has_decision`, cached version) so later phases can replace the greedy loop without rewriting this plumbing.

3. **Implement unit propagation** ✅
   - Added a `changed_pkgs` queue in `PgSolver` plus helpers so newly constrained packages are processed without scanning the entire table (`wrap/src/pgsolver/pg_core.c`).
   - Introduced full incompatibility indexing: each `PgIncompatibility` now registers per package, and `pg_unit_propagate` inspects these to derive new assignments or raise conflicts.
   - Dependency propagation is expressed through incompatibilities (`pkg@version` + `not dep-range`), so derived positive requirements come directly from PubGrub-style unit propagation.

4. **Implement conflict resolution and backjumping** ✅
   - `pg_unit_propagate` now surfaces the specific `PgIncompatibility` that became fully satisfied so the solver can reason about it instead of aborting immediately.
   - `pg_resolve_conflict` mirrors PubGrub’s resolution loop: it walks back through the trail, repeatedly resolves derived assignments with their causes (tracking the derivation tree), determines the backjump level, and produces a learned incompatibility.
   - The solver backtracks assignments down to the chosen level, attaches the learned incompatibility so future propagation can reuse it, and (for now) still reports `NO_VERSIONS` conflicts immediately because decision-making/alternative version picking has not landed yet.

5. **Implement decision making** ✅
   - `pg_make_decision` now scans every package with an outstanding positive range (from assignments) and no prior decision, using the provider to count how many viable versions remain for each candidate.
   - It prefers the most constrained package, skips versions that are already forbidden or would immediately satisfy an incompatibility, and raises a `PG_REASON_NO_VERSIONS` conflict the moment a package’s domain becomes empty.
   - Once a version is chosen, the solver records the decision and lazily generates dependency incompatibilities for that exact version so later propagation derives the required ranges.

6. **Wire Elm adapter into the true PubGrub core** ✅
   - `PgElmContext` now owns the synthetic root dependency list; `pg_elm_provider_get_dependencies` serves these when the solver asks for the root package’s edges, so the core adds dependency incompatibilities after seeding the root assignment.
   - `solver.c` converts every existing dependency map (direct/indirect/test) into `PgVersionRange`s for the root context, and package projects’ range strings are parsed via `pg_elm_parse_constraint`.
   - A shared helper in `pg_core.c` (`pg_solver_register_dependencies`) ensures both the root package and future decisions lazily emit incompatibilities `{decider, not dependency-range}` directly from the provider data.

7. **Implement strategy ladder in `solver_add_package`** ✅
   - Implemented with 3 strategies for normal adds, 1 for major upgrades.

8. **Add derivation‑based error explanations** ✅
   - Core function `pg_solver_explain_failure` implemented, but not yet wired into CLI.

---

## 5. Testing Strategy and Suggested Cases

### 5.1 Core Unit Tests (Independent of Elm)

Use a mock provider to test the core algorithm:

- **Version/range utilities**
  - `pg_version_parse`:
    - Valid strings: `"0.0.1"`, `"1.2.3"`, `"10.0.0"`.
    - Invalid strings: `"1"`, `"1.2"`, `"a.b.c"`.
  - `pg_range_*` and `pg_range_contains`:
    - Exact ranges: `[1.0.0, 1.0.0]`.
    - Until next minor / major ranges.
    - Intersections that are:
      - Non‑empty and non‑trivial.
      - Exactly one version.
      - Empty (should set `is_empty`).

- **Simple dependency graphs**
  - Acyclic graph with straightforward constraints where only the newest versions work.
  - Scenarios where:
    - Multiple versions of a dependency are allowed and solver should pick newest.
    - A conflict arises only when both of two dependers are present (eventually used to test conflict resolution).

### 5.2 Elm Adapter Tests

These tests can be done either:

- Via small C test harnesses under `wrap/test/`, or
- Manually by preparing a minimal fake Elm package directory with:
  - A tiny `registry.dat` written as JSON in the all‑packages format.
  - A couple of package folders with basic `elm.json` files.

Cases:

- **Registry parsing**
  - Ensure `pg_elm_provider_get_versions`:
    - Returns correct versions for known packages.
    - Returns them newest‑first.
    - Returns 0 for unknown packages.

- **Dependency parsing**
  - Ensure `pg_elm_provider_get_dependencies`:
    - Parses constraints like `"1.0.0 <= v < 2.0.0"` correctly.
    - Produces `PgVersionRange` that matches what `pg_range_contains` expects.

### 5.3 Integration Tests (CLI Level)

For now, these can be manual tests using `elm-wrap`:

- **Install a simple package**
  - Create a dummy project `elm.json` with no dependencies.
  - Run `wrap install <some small package>`.
  - Check:
    - `elm.json` updated with a direct dependency.
    - Package downloaded into `$ELM_HOME/0.19.1/packages/...`.

- **Respect existing exact versions**
  - Add a direct dependency in `elm.json` with a specific version.
  - Install another package that depends on this package.
  - Ensure the solver respects the existing version if it falls inside the dependency’s range.

- **Conflict scenario**
  - Prepare two packages whose constraints on a third are incompatible.
  - Try to install both; expect `SOLVER_NO_SOLUTION` and a useful explanation (once error explanations are wired into CLI).

---

## 6. Notes & Pitfalls

- The registry JSON is parsed once per `PgElmContext` and cached in `ctx->registry`; this is efficient for production.
- `PgPkgState` maintains summary information and is kept in sync with the assignment trail.
- Eventually, package projects will need careful treatment: their `elm.json` expresses ranges, not fixed versions. The current solver is biased toward “pick a single version for everything”, which is fine for app projects, but not for package project constraints.
- Keep the usage of `cJSON` consistent with `elm_json.c`; avoid creating parallel JSON parsing logic if you can reuse helpers.

As you implement new pieces, keep `pubgrub_plan.md` as the architectural reference and treat this file as the tactical checklist for coding and testing. When major design decisions change, update both documents to stay in sync. 
