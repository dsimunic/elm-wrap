# Security hardening plan (inputs + solver)

This document is a follow-up plan for security hardening work in **elm-wrap**.
The immediate goal is to reduce the risk of:

- denial-of-service (CPU, memory, disk) from adversarial inputs (especially `elm.json` and cached package metadata)
- memory-safety bugs (buffer overflow, UAF, etc.) that could theoretically escalate to RCE

## What was implemented already

Centralized limits were added to `src/constants.h`:

- `MAX_ELM_JSON_FILE_BYTES`
- `MAX_ELM_JSON_DEPENDENCY_ENTRIES`
- `MAX_ELM_JSON_VERSION_VALUE_LENGTH`
- PubGrub budgets: `PG_MAX_DECISIONS`, `PG_MAX_PROPAGATIONS`, `PG_MAX_CONFLICTS`
- Memory-growth budgets: `PG_MAX_PACKAGES`, `PG_MAX_TRAIL_ASSIGNMENTS`, `PG_MAX_INCOMPATIBILITIES`

Key entrypoints were hardened:

- bounded reads for `elm.json` parsing and downloaded `elm.json` validation
- PubGrub core loop stops with `PG_SOLVER_BUDGET_EXCEEDED` and provides a human-readable budget report

## cJSON (adopted dependency) policy

**elm-wrap** vendors cJSON (`src/vendor/cJSON.*`) and treats it as an *adopted* dependency:

- **Allocator policy:** cJSON uses the arena allocator (`arena_malloc`/`arena_free`/`arena_realloc`). Hook-based switching back to `malloc/free` is not allowed.
- **Input-size policy:** cJSON must only be fed JSON strings that were read with explicit size caps (use `file_read_contents_bounded()` and a limit from `src/constants.h`).
- **DoS guardrails:** cJSON has a nesting limit (`CJSON_NESTING_LIMIT`). If this limit is changed, treat it as a deliberate hardening decision and keep it consistent across builds.

## Threat model (practical)

### Likely attacks

- **Oversized `elm.json` / cached files**: force OOM/crash or heavy CPU parsing.
- **Adversarial dependency graphs**: trigger worst-case behavior in the solver (conflict explosion).
- **Corrupted/hostile cache contents**: same as above, but via downloaded package metadata.

### Less likely but high impact

- **RCE**: would require a memory corruption bug reachable via attacker-controlled data. These are most likely around unsafe string/formatting APIs and fixed-size buffers.

## Next research pass: “strip-cavity search” checklist

The goal of this pass is to find *all* input-adjacent paths that could be reached with attacker-controlled data and audit them for:

- unbounded reads / allocations (no file size cap)
- integer overflow (`size_t` conversions, `ftell` negative handling)
- unchecked allocation returns
- unsafe string functions (`strcpy`, `strcat`, `sprintf`, etc.)
- fixed-size buffers with attacker-controlled input (even with `snprintf`)
- direct use of forbidden allocators (`malloc`, `calloc`, `realloc`, `strdup`, `free`) outside the allocator implementation

### A. High-signal searches (run first)

1) Unsafe string functions (core audit)

- Search patterns:
  - `\bstrcpy\s*\(`
  - `\bstrcat\s*\(`
  - `\bsprintf\s*\(`
  - `\bvsprintf\s*\(`
  - `\bgets\s*\(`

2) Potentially dangerous formatting/buffer patterns

- Search patterns:
  - `snprintf\s*\([^,]+,\s*sizeof\([^\)]*\)` (ok but still check the source of interpolated strings)
  - stack buffers: `char\s+\w+\s*\[[0-9]+\]` (audit if any component is attacker-controlled)

3) Memory management violations / system allocators

- Search patterns:
  - `\bmalloc\s*\(`, `\bcalloc\s*\(`, `\brealloc\s*\(`, `\bstrdup\s*\(`, `\bfree\s*\(`
  - `realpath\([^,]*,\s*NULL\)` (implies `free()` is required)

4) Unbounded file reads

- Search patterns:
  - `ftell\s*\(` and `fseek\s*\(` pairs
  - `fread\s*\(` where allocation size is derived from `ftell`

### B. “Adjacent path” prioritization

Audit in this order (highest risk → lowest), focusing on *attacker influence*:

1) Registry/cache ingestion

- download code (`http_*`, `package_fetch*`, protocol v1/v2 fetch)
- archive extraction and selective copy
- reading cached `elm.json` / metadata used by dependency solving

2) Dependency solving

- provider implementations (dependency enumeration)
- constraint parsing / version range parsing
- any code that constructs “author/name” strings

3) CLI commands that parse JSON or traverse directories

- debug/review/publish commands that accept file paths or parse JSON

### C. Review heuristics (what to look for)

When you find a match, capture:

- function name and file
- whether the data is attacker-controlled (from file, network, env var, CLI arg)
- buffer sizes and whether limits are enforced centrally
- whether there’s a fail-closed behavior on truncation (important!)

Red flags:

- `strcpy`/`strcat` where buffer size is not obviously derived from both operands
- `sprintf` to stack buffers
- `snprintf` followed by unchecked truncation if truncation would change semantics
- `arena_malloc(len + 1)` where `len` can be negative or overflow from signed conversions
- any parsing loop that can run on arbitrarily large collections without a cap

## Concrete next-step tasks for the next chat

In the next chat, do the following in order:

1) Produce a *single consolidated list* of all `strcpy` / `strcat` / `sprintf` call sites in `src/` (exclude `src/vendor/`).

2) For each call site, classify:

- **Likely attacker-controlled** vs **local-only**
- **Stack buffer** vs **heap/arena**
- **Size derived safely** vs **size unclear**

3) Patch the highest-risk set first:

- Prefer replacing `strcpy/strcat/sprintf` with `snprintf` or size-checked helpers.
- Avoid `realpath(..., NULL)` (use caller-provided buffers sized with `MAX_PATH_LENGTH`).
- If a function must allocate based on input length, enforce limits using `src/constants.h`.

4) Run unit tests and a smoke build after each patch batch.

## Notes on scope and guardrails

- Avoid changing UX or adding features; focus on hardening.
- Keep limits centralized in `src/constants.h`.
- Prefer shared utilities (`fileutil.h`, `elm_project.h`) to prevent duplication.
- Avoid touching `src/vendor/` unless absolutely required.
