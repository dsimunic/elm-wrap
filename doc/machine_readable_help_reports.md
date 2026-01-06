# Machine-readable help reports (design)

## Motivation

**elm-wrap** currently prints help text from many command implementations scattered across the codebase. This has a few recurring problems:

- **Inconsistency**: usage/help formatting differs across commands and even across subcommands.
- **Discoverability for tests**: the current help regression tests must **parse help output** to discover subcommands, and that parsing is inherently brittle.
- **Debuggability**: when help output regresses, it is hard to answer “which command owns this string?” and “why is this command listed here?”
- **Duplication**: each command re-implements a similar pattern: check `-h/--help`, then print blocks of text.

This document proposes a system where:

1. The complete command surface area is described in a **machine-readable help registry** (MRHR).
2. The MRHR is embedded into the `wrap` binary (using the existing embedded ZIP archive mechanism).
3. A shared runtime function resolves `argv` → command path → help entry and renders help consistently.
4. The test harness reads the MRHR to discover commands (instead of parsing printed help output).

Important nuance: this is **not** a templating system (no mustache). It is a structured command definition (names, aliases, usage shapes, options, examples, notes, sections), rendered by a shared formatter.

## Goals

- **Machine-discoverable command catalog** (commands/subcommands/sub-subcommands).
- **Consistent help rendering** across the CLI.
- **Single source of truth** for what commands exist and what help is expected.
- **Tests can enumerate commands without scraping help text**.
- **Feature-flag / compiler-type aware** (e.g. Elm vs Lamdera vs wrapc; feature gates like code/policy/review/cache/publish).
- **No template engine**; only structured data + deterministic renderer.
- **Minimal runtime dependencies**: reuse existing JSON parser (cJSON) and embedded archive.

Non-goals (at least initially):

- Localization/i18n.
- Perfect parity with every existing help string on day 1.
- Rewriting every command implementation immediately.

## Current baseline (what we have today)

- Many files implement their own `print_*_usage()` and manual `strcmp(argv[i], "--help")` checks.
- Root dispatcher and some groups are hard-coded (e.g. `src/main.c` prints global usage and dispatches `package`, `application`, `repository`, etc.).
- There is already an embedded archive mechanism for data appended to the executable: `src/embedded_archive.c` uses miniz ZIP support to extract embedded files.
- There is already a regression system for help output:
  - `test/scripts/test-help-reports` recursively discovers commands by parsing help output.
  - Expected outputs live in `test/data/help-reports/`.

The MRHR system should replace the “discover commands by parsing help output” step with “discover commands by reading the registry”.

## Proposed architecture

### Overview

The MRHR system consists of four parts:

1. **Registry format** (JSON file embedded in the binary)
2. **Build-time generation/validation** (how we create the JSON, and ensure it matches the compiled command surface)
3. **Runtime lookup** (given `argv`, pick the right help entry)
4. **Runtime rendering** (deterministic formatting, one implementation)

The design is intentionally “data-driven help”, not “string templates”.

### Data flow

- Build produces `help_registry.json` (or `help_registry_v1.json`) in a predictable path.
- That file is inserted into the embedded ZIP archive appended to `wrap`.
- At runtime, `wrap` loads the registry once (lazy or at startup) when help is needed.
- Help rendering is driven exclusively by that registry.

### Embedded artifact

Use the existing embedded-archive ZIP mechanism.

- Add a new file entry inside the embedded archive, e.g.:
  - `help/help_registry.json` (or `help/help_registry.json.z` if we decide to compress inside the ZIP entry)

Notes:

- ZIP already supports compression; simplest is: store the JSON as a normal ZIP entry and let the ZIP deflate do the compression.
- This avoids adding new compression codecs at runtime.

### Runtime responsibilities

At runtime, the help system should:

1. Decide whether the user requested help:
   - Global help (`wrap --help`, `wrap -h`)
   - Group help (`wrap package --help`)
   - Leaf help (`wrap package init --help`)
2. Resolve `argv` into a **command path** (list of tokens) and look up a help entry.
3. Render a consistent help output to stdout.

The help system must also support:

- printing help from error conditions (unknown subcommand) by suggesting “Run `wrap <path> --help`”.

## Registry format (JSON)

### Why JSON?

- We already ship a JSON parser (`src/vendor/cJSON.*` and wrapper usage in `src/elm_json.c` etc.).
- JSON is easy to generate by hand and by tools.
- JSON is easy for LLMs to emit.

Alternative formats (custom binary, custom text) are possible, but JSON is the fastest path with existing code.

### High-level schema

The registry is a single JSON object with:

- `schema_version`: integer
- `program`: metadata
- `contexts`: optional constraints (compiler type, feature flags)
- `commands`: an array of command entries

Example (shape, not final):

```json
{
  "schema_version": 1,
  "program": {
    "default_name": "wrap"
  },
  "commands": [
    {
      "id": "root",
      "path": [],
      "names": ["wrap"],
      "summary": "Wrapper and package manager for Elm tooling.",
      "usage": {
        "forms": [
          { "tokens": ["{prog}", "COMMAND", "[OPTIONS]"] }
        ]
      },
      "sections": [
        {
          "kind": "commands",
          "title": "Commands",
          "items": [
            { "ref": "repl" },
            { "ref": "init" },
            { "ref": "package" }
          ]
        },
        {
          "kind": "options",
          "title": "Options",
          "options": [
            {
              "id": "global_help",
              "flags": ["-h", "--help"],
              "arg": null,
              "summary": "Show this help message"
            }
          ]
        }
      ]
    }
  ]
}
```

A few key choices:

- **`id` is stable**. It is the primary key for linking subcommands (`ref` fields).
- **`path` is explicit** (token list). This makes lookup deterministic and avoids having to interpret `names` arrays.
- **`names` includes aliases** (e.g. `application` and `app`).
- **Usage is structured** as token arrays, not a free-form string.
- **Sections are typed** (`commands`, `options`, `examples`, `notes`, etc.), not raw text blocks.

### Schema v1 (concrete)

This section defines a concrete “schema v1” that is strict enough for validation and tooling, but still practical to author by hand or generate.

The intent is not to introduce a templating language, but rather to standardize:

- which fields exist,
- which fields are required,
- how commands link to other commands,
- and which substitutions (if any) are supported.

#### Top-level object

Required:

- `schema_version` (integer): must be `1`.
- `program` (object)
  - `default_name` (string): informational only.
- `commands` (array of command objects): must include exactly one root entry with `id: "root"` and `path: []`.

Optional:

- `generated` (object): provenance for debugging
  - `by` (string): e.g. `"help-registry-gen"`, `"manual"`, `"llm"`
- `render` (object): renderer hints; must not affect lookup
  - `wrap_width` (integer): suggested default wrap width for non-TTY output

#### Command object

Required:

- `id` (string): stable unique identifier (used by `ref` links).
- `path` (array of strings): canonical command path tokens excluding program name.
  - Root is `[]`.
  - Example: `["package", "init"]`.
- `names` (array of strings): allowed tokens at this node (aliases).
  - Must be non-empty.
  - For non-root commands, must include the last token of `path`.
- `summary` (string): one-line description.
- `usage` (object)
  - `forms` (array of usage-form objects), non-empty.

Optional:

- `description` (string): additional paragraphs (no formatting tokens; renderer wraps).
- `availability` (object): gating by compiler/features (see below).
- `sections` (array of section objects): ordering is preserved by renderer.

#### Usage form object

Required:

- `tokens` (array of strings): printed with single spaces between tokens.
  - Placeholder tokens are allowed (see “Substitutions”).

Optional:

- `when` (object): conditional usage forms.
  - `compiler_types` (array of strings): any-of match.
  - `features` (object): exact boolean match.

#### Section object

Required:

- `kind` (string): one of:
  - `"commands"` (lists subcommands)
  - `"options"` (lists option flags)
  - `"examples"`
  - `"notes"`
- `title` (string): section heading.

Kind-specific payload:

- For `kind: "commands"`
  - `items` (array of command-item objects), non-empty.

- For `kind: "options"`
  - `options` (array of option objects), non-empty.

- For `kind: "examples"`
  - `items` (array of example objects), non-empty.

- For `kind: "notes"`
  - `items` (array of strings), non-empty.

#### Command item object (`kind: "commands"`)

Required:

- `ref` (string): `id` of the referenced command.

Optional:

- `display` (object): presentational rendering for the listing line
  - `tokens` (array of strings): e.g. `["init", "PACKAGE"]`.
- `summary_override` (string): if present, replaces the referenced command’s `summary` in the listing.

#### Option object (`kind: "options"`)

Required:

- `id` (string): stable identifier.
- `flags` (array of strings): non-empty. Example: `["-h", "--help"]`.
- `arg` (null or string or object):
  - `null` for flags without an argument
  - string metavariable like `"PATH"`
  - object form if we later need richer typing
- `summary` (string): one-line option summary.

Optional:

- `details` (string): additional paragraphs.
- `availability` (object): option-level gating (rare, but supported).

#### Example object (`kind: "examples"`)

Required:

- `cmd` (array of strings): command tokens.
- `summary` (string): what the example demonstrates.

Optional:

- `output` (array of strings): optional lines shown under the example (kept rare to reduce churn).

#### Availability object

Availability can exist on commands, usage forms, and options.

Optional fields (if omitted, the item is available everywhere):

- `compiler_types` (array of strings): e.g. `["elm", "lamdera", "wrapc"]`.
- `features` (object): e.g. `{ "review": true, "policy": false }`.

Semantics:

- `compiler_types`: any-of.
- `features`: all keys must match the runtime enabled/disabled value.

#### Substitutions

To avoid “missing variable” problems, substitutions should be minimal and guaranteed.

Allowed placeholder tokens (v1):

- `{prog}` → `global_context_program_name()`.

Deferred / optional (only if universally available):

- `{compiler}` → `global_context_compiler_name()`.
- `{mode}` → `global_context_mode_string()`.

No other substitutions are allowed in v1.

#### Invariants (validation rules)

Registry validation should enforce:

- `id` is unique.
- `path` is unique.
- Root exists: exactly one `path: []` entry.
- For each non-root command, `names` includes last token of `path`.
- All `ref` values resolve.
- No section has an empty item list.
- No unknown section `kind`.

### Command identity

Each command entry includes:

- `id`: stable string identifier, unique across registry.
- `path`: list of canonical tokens from root to this command (excluding program name). Example:
  - `[]` for root
  - `["package"]` for `wrap package`
  - `["package", "init"]` for `wrap package init`
- `names`: array of tokens allowed at this path node.
  - For leaf commands that have no alias, `names` is typically a single element equal to the last element of `path`.
  - For alias nodes, include both names.

Why both `path` and `names`?

- `path` makes registry entries unique and canonical.
- `names` supports aliases without duplicating full entries.

### Usage forms

Usage is a list of forms, each form is tokenized.

- Tokenization avoids ad-hoc spacing inconsistencies.
- Tokenization supports consistent wrapping/indentation rules.

Tokens can include placeholders:

- `{prog}` resolves to `global_context_program_name()`.
- Uppercase placeholders like `PACKAGE`, `PATH` are treated as metavariables.

We can optionally support a small set of formatting hints:

- `{"tokens": [...], "when": {"compiler": "elm"}}`

### Options

Options are structured objects:

- `id`: stable option identifier
- `flags`: e.g. `["-h", "--help"]`
- `arg`: either `null`, a metavariable string (`"PATH"`), or a structured argument spec
- `summary`: short description
- Optional: `details` (longer text)
- Optional: `group`: used for grouping or consistent ordering

This allows consistent rendering like:

- aligned flags
- consistent help text separators
- consistent punctuation

### Examples

Examples are structured too:

- `command`: token list (or string) rendered with `{prog}` substitution
- `summary`: one-line description

Example:

```json
{
  "kind": "examples",
  "title": "Examples",
  "items": [
    {"cmd": ["{prog}", "install", "elm/json"], "summary": "Add elm/json to your project"}
  ]
}
```

### Subcommands section

Instead of embedding formatted subcommand lines in help, we reference other commands by `id`.

This gives a *machine-readable* adjacency graph:

- You can enumerate the entire command tree without printing help.

Example:

```json
{
  "kind": "commands",
  "title": "Subcommands",
  "items": [
    {"ref": "package_install", "display": {"tokens": ["install", "PACKAGE"]}},
    {"ref": "package_init", "display": {"tokens": ["init", "PACKAGE"]}}
  ]
}
```

Notes:

- `display` is purely presentational (what gets printed in this section). It does not affect lookup.
- `ref` connects to the actual help entry.

### Feature flags and context gating

Many commands exist only when feature flags are enabled, or only for certain compiler types.

Add an optional `availability` object:

```json
"availability": {
  "compiler_types": ["elm", "lamdera"],
  "features": {"policy": true, "review": true}
}
```

Runtime filtering rules:

- If an entry’s availability does not match runtime context, it is hidden from listings.
- If the user explicitly asks help for an unavailable command, print a consistent “not available in this build” message.

The values used for filtering should come from existing global feature and compiler APIs:

- `global_context_compiler_type()`
- `feature_*_enabled()`

### Variables and the “always provide all variables” concern

To avoid “missing variables” problems:

- Keep substitution extremely small and well-defined.
- Prefer **computed values** (like `{prog}`) that are always available via global context.

Recommended substitution variables:

- `{prog}` → `global_context_program_name()`
- `{compiler}` → `global_context_compiler_name()` (optional)
- `{mode}` → `global_context_mode_string()` (optional)

Avoid variables like `{elm_home}` unless we can always compute them reliably.

If we must show values like `$ELM_HOME`, prefer rendering them as literals or via a single, centralized resolver function.

## Runtime lookup and rendering

### Lookup algorithm

Input:

- `argv` after global flags parsing (or the raw argv, depending on how we integrate)

Output:

- a command entry `id` (or not found)

Rules:

- Parse path tokens until a token is:
  - `-h` or `--help`, or
  - a flag (`-v`, `--verbose`, etc.) or positional argument (command-specific)
- For group nodes, the next token is interpreted as subcommand token.
- Aliases are resolved via `names` arrays.

This algorithm should be aligned with actual dispatch rules. (In practice, we’ll integrate the help renderer with the dispatcher so help resolution uses the same lookup table.)

### Rendering

Rendering is deterministic and shared.

- Enforce consistent ordering:
  - Usage
  - Summary/description
  - Commands/Subcommands
  - Options
  - Examples
  - Notes
- Enforce consistent indentation/alignment.
- Enforce consistent capitalization and punctuation.

Key point: rendering should not be dependent on the generator; it should be a fixed formatter with a small set of config knobs.

### Output stream

- Normal help (`--help`) prints to **stdout**.
- Errors (unknown command) print errors to **stderr**, and then a short hint to run `--help`.

This matches typical CLI conventions and makes help reports stable.

## Generation and validation strategies

You described three generation paths:

1. Hand-authored registry.
2. LLM-generated registry by scanning the code.
3. Compile-time helpers that emit a command list.

The recommended approach is a hybrid that produces strong guarantees without overengineering.

### Strategy A (recommended): “code is source of truth for command existence”, registry is source for text

- Introduce a minimal compile-time command manifest exported from the program (or produced by a build tool that compiles and runs a small “dump manifest” mode).
- Validate that:
  - every compiled command path exists in the registry
  - every registry command path is either compiled or explicitly marked as “virtual/hidden” (if we support that)

This gives you reliable enumeration and prevents drift.

Implementation options for the manifest:

- **Option A1: Static C table**: define a `CommandSpec[]` registry in code for dispatch (names, aliases, handler function, availability). This becomes the single command list.
  - Pros: maximum correctness; help and dispatch share the same list.
  - Cons: requires a refactor of dispatch code.

- **Option A2: “dump commands” runtime mode**: add `wrap --dump-command-manifest` (hidden) that prints JSON with the compiled command tree. Then a build/test step compares it to the embedded MRHR.
  - Pros: minimal refactor; keeps existing `if (strcmp(...))` dispatch initially.
  - Cons: still requires maintaining a separate dumper and ensuring it follows dispatch.

Given current code uses many `strcmp` chains, A2 is likely the lowest-friction stepping stone.

#### Proposed `--dump-command-manifest` format

The manifest’s job is to define *what commands exist in the compiled binary*, with enough structure to validate the MRHR and to enumerate commands in tests.

It is intentionally smaller than the MRHR:

- It contains **names/aliases**, paths, and availability.
- It does **not** contain descriptions, examples, long notes, or option text.

Proposed output (JSON to stdout):

```json
{
  "manifest_version": 1,
  "program": "wrap",
  "build": {
    "version": "<wrap-version>",
    "compiler_type": "elm"
  },
  "commands": [
    {
      "path": [],
      "names": ["wrap"],
      "kind": "group",
      "availability": {"compiler_types": ["elm", "lamdera", "wrapc"], "features": {}}
    },
    {
      "path": ["package"],
      "names": ["package"],
      "kind": "group",
      "availability": {"compiler_types": ["elm", "lamdera"], "features": {}}
    },
    {
      "path": ["application"],
      "names": ["application", "app"],
      "kind": "group",
      "availability": {"compiler_types": ["elm", "lamdera"], "features": {}}
    },
    {
      "path": ["package", "remove"],
      "names": ["remove", "uninstall"],
      "kind": "leaf",
      "availability": {"compiler_types": ["elm", "lamdera"], "features": {}}
    }
  ]
}
```

Field meanings:

- `manifest_version`: schema version for the manifest.
- `build`: allows tests to record the runtime environment (useful when help differs for Elm vs Lamdera).
- Each `commands[]` item:
  - `path`: canonical token path.
  - `names`: tokens accepted at this path node (aliases).
  - `kind`: `group` or `leaf` (group nodes are expected to accept a subcommand token).
  - `availability`: same semantics as the MRHR availability.

Alignment with the current dispatch style:

- The manifest can be built incrementally by each dispatcher function that currently does `strcmp(subcmd, "...")` checks:
  - A `cmd_package_dump_manifest()` function can emit `package`’s known subcommands.
  - The top-level `main` dispatcher aggregates group manifests.

This keeps A2 feasible without immediately refactoring dispatch into a unified table.

#### Validation: MRHR vs manifest

Once a manifest exists, add a validator that checks:

- For every manifest `path`, there exists exactly one MRHR command with the same canonical `path`.
- For every MRHR command (excluding explicitly marked virtual/help-only entries, if supported later), there exists a manifest entry.
- For each matching command:
  - manifest `names` is a subset of MRHR `names` (or exactly equal, depending on strictness).
  - availability constraints are compatible.

This validation is what prevents drift and makes “LLM-generated MRHR” safe.

### Strategy B: Pure generator based on scanning code

- Scan sources for patterns like `if (strcmp(subcmd, "X") == 0)`.
- Infer command names/aliases.

This will work surprisingly well for many cases but is fragile:

- It can miss dynamic dispatch or feature gates.
- It can misread tokens inside unrelated logic.

This is still useful as an **assistant** (LLM or script) to bootstrap the MRHR, but should not be the only correctness mechanism.

### Strategy C: LLM-assisted authoring

This is excellent for populating:

- summaries
- examples
- option descriptions
- longer “notes”

But should be backed by machine validation:

- command existence
- alias correctness
- option flags

### Validation checks (build/test)

At minimum, validate:

- JSON schema version is known.
- Every command has:
  - `id`, `path`, `names`, `summary`, at least one `usage` form.
- No duplicate `id`.
- No duplicate canonical `path`.
- Every `ref` points to an existing `id`.
- Availability expressions are syntactically valid.

Additional validation (recommended):

- The compiled command manifest matches the registry (bidirectional check).

## Testing and help-report workflow

### Replace “discover commands by parsing help output”

Update `test/scripts/test-help-reports` to:

1. Read the registry list of commands.
2. For each command path, run `... --help` and compare output.

This removes the brittle `extract_commands()` parsing logic.

### Canonical command list vs alias list

The registry should distinguish:

- **canonical path**: a single path per command (used for tests)
- **aliases**: alternative spellings (optional to test separately)

Testing approach:

- Always test canonical path help output.
- Optionally test alias paths and assert they render the same help (or redirect to canonical).

### Output stability

Renderer should be deterministic:

- stable spacing and wrapping rules
- stable ordering

This should significantly reduce churn in `test/data/help-reports/`.

## Migration plan

A practical migration that avoids a massive refactor:

1. **Introduce the MRHR file and renderer** but keep existing per-command printing for now.
2. Add a new code path so `--help` prefers MRHR if available; otherwise falls back to existing print functions.
3. Migrate one command group at a time:
   - root help
   - `package` group
   - `debug` group
   - etc.
4. Once coverage is high enough, flip the default to MRHR-only.
5. Remove old printing functions gradually.

This incremental approach lets you keep shipping.

## Open questions / design decisions to confirm

1. **Where should help live in the embedded ZIP?**
   - Proposed: `help/help_registry.json`

2. **Should help rendering depend on terminal width?**
   - For stable tests, default to a fixed wrap width unless TTY detected.

3. **Should help output include feature-gated commands that are disabled?**
   - Proposed: hide by default; show in a “Disabled commands” section only if explicitly requested (but that might be extra UX we don’t need yet).

4. **How to represent “command group nodes” vs “leaf commands”?**
   - Proposed: both are commands; groups are those that have a `commands` section.

## Risks and mitigations

- **Registry drift from dispatch**
  - Mitigation: add compiled manifest and validate.

- **Too many variables / missing substitutions**
  - Mitigation: keep substitutions minimal (`{prog}` only initially).

- **Feature/Compiler differences create multiple help variants**
  - Mitigation: availability gating in registry; renderer filters.

- **Large registry increases startup time**
  - Mitigation: only load registry when `--help` is requested (lazy).

## Summary

A machine-readable help registry embedded in the `wrap` binary, combined with a shared lookup + renderer, should:

- make command enumeration reliable,
- make help output consistent,
- and make help regression tests much simpler.

The key is to back the registry with **machine validation against the compiled command surface**, while using the registry as the authoritative home for descriptions, options, and examples.
