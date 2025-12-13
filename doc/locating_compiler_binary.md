# Locating a Compiler Binary: design and implementation guide

This document explains a robust, portable approach for locating and identifying a compiler binary (or other CLI tool) your program needs to call. It uses a small number of steps that are easy to implement in C or many other languages, and are compatible with common expectations for command-line tools.

The general goals are:
- Respect a user-specified path through an environment variable.
- Fall back to a PATH search for the canonical binary name when no override is provided.
- Determine the compiler's name (basename) and its semantic version by invoking it, so the program can choose behavior depending on version and name.
- Provide clear error messages if the binary cannot be located or is not runnable.

Use cases covered:
- Allow users to run a specific compiler binary (invoking a custom fork, different language implementation, or local build).
- Enable repository or package selection logic based on compiler name + version.
- Detect capabilities or help commands based on the compiler name.

Design and assumptions
----------------------
- Environment variable: the consumer program accepts an environment variable to be set by users. For example, `MYAPP_COMPILER_PATH`. If set, treat it as highest priority.
- Default binary name: if no env var is set, search for a default name such as `elm` (or `mycompiler`) in the PATH.
- Version check: verify the compiler version by running `compiler --version` and parsing a `X.Y.Z` style output (or an appropriate detection pattern).
- Platform assumptions: example code is POSIX-affine; a cross-platform implementation should account for Windows-specific PATH semantics and executable extensions.

Step-by-step algorithm
----------------------
1. Environment variable lookup
  - Read `MYAPP_COMPILER_PATH` (or another name chosen by your project) using `getenv`.
  - If present and non-empty, optionally expand a leading `~` (tilde) into the user's home directory.
  - Normalize the path (`realpath` or equivalent) if your program needs an absolute path.
  - If the path points directly to a binary file, verify it exists and is executable. On POSIX, use `stat()` and verify `S_ISREG()` and `st_mode & S_IXUSR` / `access(path, X_OK)`; on Windows consider `GetFileAttributes()` and the PATHEXT extension list.
  - If the given path is invalid, produce a clear error message and fail or fall back to PATH search depending on your policy.

2. BASENAME / Determining the compiler name
  - If an environment path was supplied, use the path's basename for the compiler name (e.g. `/some/path/elm` => `elm`).
  - If the environment variable is not set, use a sensible default (e.g. `elm`) as the compiler name.
  - Use this name to determine runtime behavior (e.g., whether the compiler is `lamdera` or `wrapc`), and to form repository paths like `ROOT/<compiler-name>/<version>` when applicable.

3. PATH search fallback (if no env var set)
  - Get the PATH environment variable and split it on `:` (POSIX) or `;` (Windows) into directories.
  - For each directory, construct a candidate path like `dir/<default-binary-name>`; on Windows iterate over PATHEXT extension mappings to try `mycompiler.exe`, `mycompiler.bat`, etc.
  - Check `stat`/`access` for each candidate; if a candidate is found that is an executable regular file, use that path.
  - If no candidate is found, return `NULL`/error so the caller can decide what to do (e.g., print install instructions).

4. Determining the compiler version
  - Once a path is determined, run a version command such as `binary --version` and capture the first line of output. Using `popen()` or spawning a process that captures stdout is typical.
  - Protect against user-controlled inputs: avoid shell interpolation when invoking the command (prefer `execv()` or a safe API), or if you must use a shell, ensure the string is safely formatted and only contains what you expect.
  - Normalize output: strip newline and whitespace.
  - Parse a canonical version format (commonly `X.Y.Z`). Use a strict parser to avoid accepting malformed strings.
  - If parsing fails, treat it as unknown version; some tools may accept partial formats, but prefer strict version checks for correctness.

5. Using compiler name + version
  - If the caller needs different behavior by compiler type, map the basename to a known compiler type (e.g., `if basename == "elm" -> ELM` otherwise `LAMDERA`, etc.).
  - Use the version to select compatibility features, load repository directories (e.g. `ROOT/<compiler-name>/<version>`), or adjust commands.

Implementation hints and details
--------------------------------
- Tilde expansion: Implement `expand_tilde()` that expands `~` to `$HOME` for user input. This is helpful when users put `~/bin/elm` in env vars.
- Checking executable: `stat()` and `S_IXUSR` are portable to POSIX; on Windows consider `access(path, X_OK)` and PATHEXT handling.
- PATH parsing: Don't rely on `strtok()` mutating a static copy; instead make a duplicated string and iterate safely.
- Memory management: carefully manage any dynamic allocation you create; if you use an arena allocator, follow its API.
- Error messages: If env var is set but the binary does not exist, provide a clear message like "The compiler was not found at the path specified by MYAPP_COMPILER_PATH".

Security and safety
-------------------
- Avoid executing untrusted shells. If you must call a command via a shell string, ensure it's narrow and controlled (e.g., escape or whitelist). Prefer `execv`, `fork/exec`, or APIs that avoid the shell.
- When capturing output from `popen()` or equivalent, be careful of arbitrary output size; limit buffer size and ignore excess or truncate.
- If the path is untrusted, do not use it to write files without validation.

Platform notes
--------------
- Path separators:
  - POSIX: PATH uses `:` separator.
  - Windows: PATH uses `;` and requires handling PATHEXT to find executables.
- Executable detection:
  - POSIX: use `stat()` and check `S_ISREG(st_mode)` and `access(path, X_OK)`.
  - Windows: file may have one of several executable extensions; check PATHEXT and try candidate filenames with those extensions.

Example C-ish pseudo-implementation outline
-----------------------------------------
(The following is a conceptual outline and intentionally simplified.)

1) Expand env var and check:

- read env var `MYAPP_COMPILER_PATH`
- if present and non-empty:
    - expanded = expand_tilde(value)
    - if `file_exists(expanded)` and `is_executable(expanded)`: return strdup(expanded)
    - else: log error

2) Search PATH:

- path_env = getenv("PATH")
- for dir in split(path_env, PATH_SEP):
    candidate = join(dir, default_binary_name)
    if file_exists(candidate) && is_executable(candidate):
        return strdup(candidate)
- return NULL

3) Determine name & version:
- name := basename(compiler_path) if env var set, otherwise default name
- run `[compiler_path, "--version"]` capturing output
- trim output line and parse `X.Y.Z` with `sscanf` or regex; validate components

Testing recommendations
-----------------------
- Test cases for:
  - Env var set to absolute path (valid, invalid, not executable).
  - Env var set to path with tilde (`~/bin/compiler`).
  - Env var not set, PATH has an executable.
  - PATH does not contain the binary.
  - Version output is valid `X.Y.Z` and invalid/empty values.
- Unit tests can stub out `getenv`, `stat`, and process spawn APIs, or use temporary files and set the PATH appropriately in test harness.

Tips for robust operation
-------------------------
- Prefer returning the absolute path from your API so downstream code has a concrete path to invoke.
- Keep a clear separation between locating a binary (resolves path) and probing it (version checks and capability checks). This keeps the code easier to test and reason about.
- Provide alternate fallbacks; on failure, choose either to error out early with actionable advice for the user, or continue with degraded behavior if possible.

Further refinements
-------------------
- Support for a complementary environment variable that sets the preference of binary name to use (e.g., `MYAPP_COMPILER_NAME`) rather than relying solely on basename.
- Cache detection results to avoid IO for multiple calls during the process lifetime.
- Consider supporting a `--compiler` command-line override which should take priority over environment variables.

Appendix: Example behavior checklist
-----------------------------------
- Environment variable set -> use it (after expand + validate).
- Environment variable set & invalid -> show explanatory message.
- Environment variable not set -> search PATH -> use found one.
- No binary found -> return NULL and show fallback/installation guidance.
- Compiler path -> extract basename to determine name.
- Version check -> `binary --version`, parse `X.Y.Z` exactly.

This approach should help your project implement a robust compiler detection mechanism that is easy for users to override while preserving clear behavior and diagnostics.
