# Logging

Logging in in this project is treated like a power tool: it belongs in one place, it has safety guards, and you keep your hands off the spinning blades (`printf`).

The core idea is simple:

- Application code emits diagnostics via the `log_*` macros.
- The logging backend lives in one centralized header/implementation pair.
- Verbosity is controlled by CLI flags (`-v`, `-vv`) and occasionally by temporarily raising the log level for mandatory UX output.
- Fast builds compile out high-volume debug/trace logging so hot paths stay hot.

---

## High-level overview

### Why there is a logging system at all

- **Consistency**: every message is formatted the same way and goes to the same stream.
- **Separation of concerns**: diagnostics go to stderr; program output goes to stdout.
- **Verbosity control**: the user asks for more output (`-v`, `-vv`) and gets it; otherwise they do not.
- **Performance**: debug/trace logging can be removed entirely in fast builds.

### The API surface

The logging API is centralized in:

- `src/shared/log.h` (macros + helpers)
- `src/shared/log.c` (global level + initialization)

Application code uses:

- `log_error(...)`
- `log_warn(...)`
- `log_progress(...)`
- `log_debug(...)`
- `log_trace(...)`

Plus helpers:

- `log_init(int verbosity)`
- `log_set_level(LogLevel level)`
- `log_is_debug()`, `log_is_trace()`
- `DBGV_UNUSED` (for variables used only in debug builds)

The “`LOG_` family” in this codebase is primarily:

- `LOG_LEVEL_*` (the ordered severity levels)
- subsystem wrappers that look like `VM_LOG_DEBUG(...)` / `VM_LOG_TRACE(...)` (used in VM hot paths)

---

## Reference: log levels and routing

### Log levels

Logging is governed by a single global level `g_log_level`.

Levels are ordered (low → high):

- `LOG_LEVEL_ERROR`
- `LOG_LEVEL_WARN`
- `LOG_LEVEL_PROGRESS`
- `LOG_LEVEL_DEBUG`
- `LOG_LEVEL_TRACE`

A message at level X is emitted if and only if `g_log_level >= X`.

### Streams and formatting

- `log_error(...)` writes to **stderr** and prefixes with `[ERROR]`.
- `log_warn(...)` writes to **stderr** and prefixes with `[WARN]`.
- `log_progress(...)` writes to **stderr** with **no prefix**.
- `log_debug(...)` writes to **stderr** and prefixes with `[DEBUG]`.
- `log_trace(...)` writes to **stderr** and prefixes with `[TRACE]`.

`log_debug(...)` and `log_trace(...)` flush stderr after printing.

### Newlines

Each logging macro appends exactly one trailing newline.

- Do not include `\n` inside format strings.

---

## Reference: verbosity selection (`-v`, `-vv`) and mandatory output

### CLI flags and `log_init`

The CLI parses verbosity flags and calls `log_init(verbosity)`.

Verbosity mapping is:

- `verbosity == 0` → `g_log_level = LOG_LEVEL_ERROR`
- `verbosity == 1` (aka `-v`) → `g_log_level = LOG_LEVEL_DEBUG`
- `verbosity >= 2` (aka `-vv`) → `g_log_level = LOG_LEVEL_TRACE`

Implication: in default mode, **only errors print**. Warnings, progress, debug, and trace output are suppressed unless verbosity is enabled or the program temporarily raises the level.

### Mandatory UX output (even when not verbose)

Some output is considered “must be visible” regardless of the current log level. The pattern is:

1. Save the current level.
2. Temporarily set level to `LOG_LEVEL_PROGRESS`.
3. Print user-facing lines via `log_progress(...)`.
4. Restore the previous level.

This is used for things like:

- Help/usage output.
- Timing summaries (when summaries are configured to show).

This pattern ensures `-h` CLI flag works even though normal runs default to `LOG_LEVEL_ERROR`.

### What gets gated behind CLI verbosity

Use these conventions:

- `log_error`: user-actionable failures and correctness issues. Always visible.
- `log_warn`: “might be surprising, but continuing”. Visible only if `g_log_level >= LOG_LEVEL_WARN`.
	- In `frankie` runs, that effectively means “visible under `-v` or `-vv`” unless the level is temporarily raised.
- `log_progress`: stable, user-facing status lines (usage text, summaries, “server listening”, etc.).
	- In `frankie` runs, progress lines are typically printed only when the program explicitly raises the level to `LOG_LEVEL_PROGRESS`.
- `log_debug`: diagnostics for developers; enabled by `-v`.
- `log_trace`: very chatty diagnostics; enabled by `-vv`.

### Interaction with timing (`-t`)

`-t/--timing` enables timing collection.

- With `-t` **and** `-v`, per-stage details are emitted via `log_debug(...)`.
- With `-t` **without** `-v`, timing details are deferred to a summary printed at exit via `log_progress(...)` (using the “temporarily raise to PROGRESS” pattern).

### Interaction with `-q/--quiet`

`-q/--quiet` suppresses passing `--verbose` to the external compiler.

- It does **not** directly change `g_log_level`.
- It is orthogonal to `-v/-vv`.

---

## Reference: centralization (how printf stays out of your code)

### Single choke point

Application code should NEVER call `printf(...)` / `fprintf(...)` for diagnostics.

Instead:

- Include `log.h`.
- Emit diagnostics via `log_*`.

This keeps formatting, stream choice, and verbosity gating centralized.

### The intentional separation: diagnostics vs program output

- Diagnostics belong on stderr via `log_*`.
- Program output (what the executed program comunicates to the user) belongs on stdout/stderr and is governed by host capabilities.

This separation prevents debug logs from corrupting program output streams.

### Verification (no new printf/fprintf)

Use grep to ensure application code does not introduce direct printing:

```bash
grep -r --include="*.c" '\\bprintf\\s*(' src/
grep -r --include="*.c" '\\bfprintf\\s*(' src/ | grep -v log.h
```

Vendor code is allowed to do its own thing; keep the “no `printf`” rule focused on our application code.

---

## Reference: compile-time stripping for fast builds

### `FAST_MODE` as the master switch

When `FAST_MODE` is defined, `src/common/log.h` compiles out high-volume logging:

- `log_debug(...)` becomes `((void)0)`.
- `log_trace(...)` becomes `((void)0)`.
- `log_is_debug()` and `log_is_trace()` become `false`.

This is designed to remove overhead from hot paths (VM execution, kernel dispatch, tight loops).

### `DBGV_UNUSED`

In fast mode, code often has locals that exist only to feed debug logs. `DBGV_UNUSED` is provided to mark such locals and avoid warnings.

- In `FAST_MODE`, `DBGV_UNUSED` expands to `__attribute__((unused))`.
- Otherwise it expands to nothing.

### Build system integration

The Makefile supports a fast configuration:

- `FAST_MODE=1 make` (enables `-DFAST_MODE -DCOMPUTED_GOTO`)
- `make rebuild-fast` (convenience target)

Fast mode is a production-oriented configuration: it trades debuggability for throughput and lower overhead.

### Subsystem wrappers (example: VM)

Hot subsystems frequently add their own wrappers so the callsites stay clean and the rules stay obvious.

Example pattern:

- `VM_LOG_DEBUG(...)` expands to `log_debug(...)` guarded by `log_is_debug()`.
- In fast mode, `VM_LOG_DEBUG(...)` becomes a no-op.

Use this pattern when a subsystem has logging that must be both:

- extremely cheap when disabled, and
- easy to delete/compile out under `VM_FAST_MODE`.

---

## Guidance: how new code should structure its logging

### Pick the level based on who needs to read it

- Use `log_error` for failures that the user can act on.
	- Include context (file path, option name, system error string) and what the user should change.
- Use `log_progress` for stable, user-facing output.
	- Prefer `log_progress` over `log_warn` when the message must be visible without `-v`.
- Use `log_warn` for “continuing, but here’s something surprising”.
	- In `frankie` default runs warnings are not visible; do not rely on warnings to communicate required actions.
- Use `log_debug` for developer diagnostics.
	- Assume it is enabled only under `-v`.
- Use `log_trace` only for very high-volume details.
	- Assume it is enabled only under `-vv`.

### Keep the fast path fast

- Do not build expensive strings unconditionally.
	- If a log line needs heavy computation, wrap it:

```c
if (log_is_debug()) {
		/* compute expensive details */
		log_debug("...");
}
```

- For tight loops or VM/kernels, prefer a subsystem wrapper like `VM_LOG_DEBUG(...)`.

### Make messages composable and grep-friendly

- Start messages with a stable prefix for the subsystem (`"trace:"`, `"loader:"`, `"vm:"`, etc.).
- Put variable details at the end.
- Keep each message to one line.

### Do not print newlines or double-prefix

- Logging macros add a newline.
- `log_error/log_warn/log_debug/log_trace` already add `[LEVEL]`.
	- Do not include your own `[ERROR]`-style tags inside the message.

### Prefer “raise to PROGRESS temporarily” for must-show blocks

When emitting a help block, summary, or other mandatory UX text:

- Save the current `g_log_level`.
- `log_set_level(LOG_LEVEL_PROGRESS)`.
- Print via `log_progress(...)`.
- Restore the previous level.
