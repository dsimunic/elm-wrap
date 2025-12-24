# Writing secure code in **elm-wrap**

This document is the **non-negotiable** guidance for humans and coding LLMs when generating or modifying code in **elm-wrap**.

If you do not follow these rules, your code is considered incorrect even if it compiles.

## Scope and intent

You MUST:

- Prefer fixes at the root cause over cosmetic patches.
- Keep changes minimal and in-scope.
- Keep all hardening limits centralized in `src/constants.h`.
- Preserve the existing UX and CLI behavior unless explicitly instructed otherwise.

You MUST NOT:

- Add new features “while you’re here.”
- Change user-facing output, commands, flags, or defaults as part of “hardening.”
- Touch vendored code under `src/vendor/` unless explicitly required.

## Memory management (arena allocator)

You MUST:

- Use **only** the arena wrappers from `src/alloc.h`:
  - `arena_malloc`, `arena_calloc`, `arena_realloc`, `arena_strdup`, `arena_free`
- Treat allocation failures as real and possible. Check returns and fail safely.

You MUST NOT:

- Call standard allocators in application code:
  - `malloc`, `calloc`, `realloc`, `strdup`, `free`
- Introduce hidden allocations such as `realpath(..., NULL)` (it forces `free()` semantics).

You MUST:

- Track `count` and `capacity` for any growable array.
- Grow capacity before writing.
- Avoid “silent drop” patterns (where new items are ignored when capacity is full).

## File I/O and input-size limits

All input-adjacent file reads MUST be bounded.

You MUST:

- Read attacker-influenced files using `file_read_contents_bounded(path, max_bytes, out_size)`.
- Pick `max_bytes` from `src/constants.h`. If none exists, add one with a clear comment.
- Treat “too large” as a hard failure (return NULL / error). Do not truncate-and-continue.

You MUST NOT:

- Implement “read whole file” via `fseek`/`ftell`/`fread` unless you can prove the size is bounded.
- Use unbounded helpers as an easy shortcut.

You MUST:

- Validate signed/unsigned conversions:
  - Never cast a negative `ftell()` result to `size_t`.
  - Never compute allocation sizes from attacker-controlled integers without overflow checks.

## String and formatting safety

You MUST:

- Avoid unsafe string APIs in non-vendor code:
  - `strcpy`, `strcat`, `sprintf`, `vsprintf`, `gets`
- Prefer:
  - `snprintf` with correct buffer sizes
  - `memcpy` / `memmove` when length is known and bounded

You MUST:

- Check for truncation when truncation would change semantics.
  - If truncation could cause incorrect paths, incorrect hashes, incorrect protocol messages, or misleading output, you must fail closed.

You MUST:

- Use centralized buffer constants from `src/constants.h`.
- Avoid magic numbers for buffer sizes and initial capacities.

## cJSON (adopted dependency) rules

cJSON is vendored (`src/vendor/cJSON.*`) and treated as an adopted dependency.

You MUST:

- Keep cJSON allocation on the arena allocator (no stdlib allocator paths).
- Only parse JSON that was read with explicit size caps (bounded reads + a constant limit).
- Treat changes to `CJSON_NESTING_LIMIT` as a deliberate DoS-hardening decision.

You MUST NOT:

- Add parallel JSON parsing logic when existing helpers can be reused.

## Vendored code policy (`src/vendor/`)

You MUST:

- Avoid editing vendored dependencies unless explicitly requested or there is no viable alternative.
- If you must edit vendor code, you must:
  - Keep the diff minimal
  - Document what was changed and why
  - Preserve upstream semantics unless the change is a security fix

## Error handling and “fail closed” behavior

You MUST:

- Prefer returning an error (or NULL) over continuing with partial/invalid data.
- Ensure clean-up is correct under the arena model.
- Log errors where the caller needs diagnosability, but do not leak secrets.

You MUST NOT:

- Continue after parse failures, truncated reads, or invalid formats.

## Build and tooling rules

You MUST:

- Keep the build warning-clean (`-Werror` means warnings are failures).
- Rebuild after changes with `make all`.

If you modify `publish docs` behavior or any docs generation path, you MUST:

- Validate using `tools/validate-docs <author/package/version>`.

Shell rules:

- Interactive terminal commands MUST be fish-compatible.
- Scripts under `scripts/` MUST be bash.
- If you create/modify a bash script, you MUST run shellcheck.

## Makefile correctness

You MUST:

- Use a literal tab for Makefile recipe lines.
- Avoid introducing fragile link-order dependencies; if an object begins using a shared helper, update all tool/link targets that include it.

## Security hardening checklist (for new code)

Before you consider a change “done,” you MUST confirm:

- All file reads from user input / network / caches are bounded with a `src/constants.h` limit.
- No unsafe string functions were introduced.
- No forbidden allocators were introduced.
- All allocations are checked.
- All loops over attacker-controlled data have a cap or budget.

## What is explicitly out of scope

You MUST NOT:

- “Refactor for style,” “modernize,” or “clean up” unrelated code.
- Add optional features, new commands, or additional UI output as part of hardening.
- Replace dependencies or introduce new libraries unless explicitly requested.
