# Remote Install and Cache Implementation

This document explodes steps 1–3 of the install algorithm (see `wrap/doc/install_implementation.md`) into implementation-level detail. It focuses on the remote registry, cache initialization, and dependency discovery logic, with concrete C design guidance. The described components sit between the CLI front-end (`install.c`) and the solver and reuse infrastructure under `wrap/src` (`cache.c`, `elm_json.c`, `elm_json.h`, etc.).

---

## Step 1 — Initialize Environment, Registry, and HTTP Session

### 1.1 Install Environment Structure
- Introduce an `InstallEnv` struct that aggregates:
  - `CacheConfig *cache` (already defined in `cache.h`)
  - `Registry *registry` (see §1.3)
  - `bool offline`
  - `CurlSession` (wrapper described in §1.2)
  - `char *registry_etag` (optional for future conditional requests)
  - `size_t known_version_count` (from registry cache header)
- Provide helper lifecycle functions:
  - `InstallEnv *install_env_create(void);`
  - `bool install_env_init(InstallEnv *env);`
  - `void install_env_free(InstallEnv *env);`

`install_env_init` performs:
1. `cache_config_init`, `cache_ensure_directories`.
2. `curl_global_init(CURL_GLOBAL_DEFAULT)` and creation of a reusable easy handle.
3. Loading the registry cache from `cache->registry_path`. Failure to load triggers a full HTTP fetch (§1.4).
4. Read package registry url from `ELM_PACKAGE_REGISTRY_URL`. Default to `https://package.elm-lang.org` if not defined.
5. `env->offline = !curl_session_can_connect(session)` so that later stages know whether to attempt network fetches. The helper runs a quick HEAD request against `https://package.elm-lang.org/health-check` (or `/all-packages` if the former is unavailable) with a 2-second timeout.

### 1.2 HTTP via libcurl
- Create a lightweight abstraction in `http_client.c` (or reuse an existing module) to hide libcurl details.
- Initialization:
  ```c
  typedef struct {
      CURL *handle;
      long timeout_ms;
      struct curl_slist *headers;
  } CurlSession;
  ```
  - Configure once: `CURLOPT_USERAGENT` (`"Elm/0.19.1 (libcurl)"`), `CURLOPT_FOLLOWLOCATION`, `CURLOPT_ACCEPT_ENCODING`, `CURLOPT_NOSIGNAL` (for multi-thread safety), TLS verification defaults, and TLS CA path detection (respect `CURL_CA_BUNDLE`, `SSL_CERT_FILE` env vars).
  - Use a shared write callback:
    ```c
    typedef struct {
        char *data;
        size_t len;
    } MemoryBuffer;
    
    static size_t write_cb(void *contents, size_t sz, size_t nmemb, void *ud);
    ```
    The callback reallocs `data`, appends incoming bytes, and null-terminates before returning `sz * nmemb`.
- `http_get_json(CurlSession*, const char *url, MemoryBuffer *out)` configures `CURLOPT_URL`, pointers to `write_cb`, resets previous response buffers, and runs `curl_easy_perform`.
- Wrap libcurl status handling: capture `CURLcode`, HTTP response code via `curl_easy_getinfo(CURLINFO_RESPONSE_CODE, …)`, and convert into an `HttpResult` enum so higher layers distinguish transient network errors, HTTP 4xx, and 5xx.
- For concurrency, calls remain sequential inside `elm install`; keep one easy handle, resetting options via `curl_easy_reset` between requests.

### 1.3 Reading `registry.dat`
- File format reference: `wrap/doc/file_formats.md §1`.
- Add `registry.h`/`registry.c` with:
  ```c
  typedef struct {
      char *author;
      char *name;
      Version *versions;
      size_t version_count; // includes newest + previous list
  } RegistryEntry;
  
  typedef struct {
      RegistryEntry *entries;
      size_t entry_count;
      size_t capacity;
      size_t total_versions; // mirrors header count
  } Registry;
  ```
- `Registry *registry_load_from_dat(const char *path, size_t *known_count);`
  - Use `FILE *f = fopen(path, "rb")`; return `NULL` if file absent.
  - Read header: `uint64_t total_versions = read_be64(f);` (helper `bool read_u64_be(FILE*, uint64_t *)`).
  - Read map size: `uint64_t entry_count = read_be64(f);`.
  - For each entry, decode package name: author length (byte), author bytes, project length, project bytes.
  - Read `KnownVersions`: first version is newest, followed by `uint64_t prev_len`, then list.
  - Allocate arrays with overflow checks (e.g., ensure `entry_count < SIZE_MAX / sizeof(RegistryEntry)`).
- Provide lookup helpers:
  - `RegistryEntry *registry_find(Registry*, const char *author, const char *name);`
  - `bool registry_contains(Registry*, const char *author, const char *name);`
- `registry_dat_write` (see §1.5) mirrors the read logic but writes using big-endian encoding.

### 1.4 Fetching Registry Data
Two fetch paths depending on cache presence:

**Full Fetch**
1. When `registry.dat` is missing or corrupted, call `GET https://package.elm-lang.org/all-packages`.
2. Parse JSON via `cJSON_Parse` (the project already depends on cJSON through `elm_json.c`). Iterate key/value pairs, convert version strings to `Version` objects (split by dots, convert to integers).
3. Build `Registry` from JSON data. While doing so, fill `registry->total_versions` with the total number of version strings encountered.
4. Persist binary cache via `registry_dat_write`.

**Incremental Update**
1. When cache exists, call `GET https://package.elm-lang.org/all-packages/since/{known_count}` with `known_count = registry->total_versions`.
2. The response is an array of `"author/package@1.2.3"` strings; parse by splitting on `'@'`.
3. Append new versions to relevant `RegistryEntry`. Maintain descending order by inserting at front if new > newest (semver compare by major/minor/patch). Update `registry->total_versions`.
4. If the array is empty, skip writing; else, persist to disk.

**Persistence**
- `bool registry_dat_write(const Registry*, const char *path)`:
  - Write to a temporary file (`registry.dat.tmp`), flush, `fsync`, then `rename` to avoid corruption.
  - Use helper `write_u64_be`, `write_u8`.
  - Ensure directories already exist via `cache_ensure_directories`.

### 1.5 Connection Mode and Error Handling
- If libcurl fails (timeout, DNS) when fetching registry data, set `env->offline = true` and keep using cached registry. Errors propagate with actionable messages:
  ```
  Failed to update package registry (using cached data):
    URL: https://package.elm-lang.org/all-packages/since/16446
    curl: Could not resolve host: package.elm-lang.org
  ```
- When no cache and offline, abort install with guidance to run again online.
- Provide a `--offline` CLI flag that forces `env->offline = true` and bypasses HTTP. In that case, skip incremental updates and rely solely on disk data; CLI should warn if packages are missing.

---

## Step 2 — Read and Prepare `elm.json` Outline

### 2.1 Locate Project Root
- Use existing CLI logic to locate `elm.json` (walk upward from CWD until found or root). This should populate `InstallContext.project_root` and `InstallContext.elm_json_path`.
- The current implementation assumes `elm.json` is in the current working directory. The proposed design extends this to walk upward to find the project root.
- Provide an error message mirroring Elm's: "`elm.json` not found. Run `elm init` in the project root."

### 2.2 Parse JSON Into `ElmJson`
- Reuse `elm_json_read` (`wrap/src/elm_json.c`) to obtain project metadata. This function already captures:
  - `ElmProjectType`
  - Dependency maps (`PackageMap`) for applications: `dependencies_direct`, `dependencies_indirect`, `dependencies_test_direct`, `dependencies_test_indirect`
  - For packages: `package_dependencies` and `package_test_dependencies`
- The existing implementation handles parsing of `test-dependencies` for both application and package projects.
- The existing implementation uses `ElmJson` directly without the `InstallOutline` wrapper struct. The proposed `InstallOutline` provides a more focused interface for install operations.

### 2.3 Updating `elm.json`
- When Step 3 promotes dependencies or the solver later injects new ones, modify the `PackageMap` inside `ElmJson`.
- `elm_json_write` already exists; verify it emits maps with alphabetical keys, since Elm CLI expects deterministic ordering.
  - If not currently alphabetical, add `package_map_sort` (qsort by `author/name`) before serialization.
- Provide `elm_json_promote_package` helper (already implemented in `wrap/src/elm_json.c`) that:
  1. Removes entry from current map.
  2. Adds it to `dependencies_direct` (or `package_dependencies` for packages).
  3. Records `PromotionType` to inform user messaging ("Moved from test dependencies to direct dependencies.").
- After modifications, call `elm_json_write` with the original path. Consider writing to `elm.json.tmp` then renaming (mirroring registry write safety).

### 2.4 Outline Validation
- Validate that direct dependencies exist inside `PackageMap`; otherwise report corrupted `elm.json`.
- Extract CLI target package (Step 3):
  - Validate format `author/package`.
  - Ensure package is not the same as the project itself when in package mode.
- Build a `Constraints` map that the solver will later consume: each known dependency becomes a constraint with its locked version. Step 2 records this map but defers solver invocation.

---

## Step 3 — Check for Existing Package and Registry Presence

### 3.1 Application Projects
- Implement `PromotionResult check_app_package(InstallOutline*, const PackageId *target);` where `PackageId` splits author/name.
- Logic:
  1. Search `dependencies_direct`. If found → `status = INSTALL_ALREADY_DIRECT`.
  2. Else search `dependencies_indirect`. On hit, remove from `indirect`, add to `direct`, record `PROMOTION_INDIRECT_TO_DIRECT`.
  3. Repeat for `test_direct` and `test_indirect` (for the latter, promote into `dependencies_direct` after removing from both test maps).
  4. Each promotion sets a flag that Step 5 uses to rewrite `elm.json` immediately (no solver call needed).
  5. Return `INSTALL_NEEDS_SOLVER` when package is absent everywhere; continue with registry lookup.
- While promoting, ensure `PackageMap` reallocation errors bubble up; return a dedicated failure message (“Failed to promote package due to memory allocation error.”).

### 3.2 Package Projects
- Provide analogous `check_pkg_package`:
  1. Search `package_dependencies`.
  2. Search `package_test_dependencies`, moving entries into main deps.
  3. Follow-on solver is invoked only when the package is new.
- Reject attempts to install own package (matching `elm.json` `"name"`): return error mirroring Elm.

### 3.3 Registry Lookup and Version Availability
- For packages not already installed, verify they exist in the registry loaded in Step 1.
  - Use `registry_find`. If `NULL`, compute suggestions:
    - Walk all entries, compute Levenshtein distance or simple prefix matches.
    - Return up to 4 suggestions sorted by similarity.
  - Error message sample:
    ```
    I cannot find package `author/project`.
    Maybe you want one of these?
        author/projection
        authorx/project
    ```
- When a registry entry exists, capture its version list for the solver:
  - Convert to solver `Constraint` objects. The initial constraint for the target package is "anything", but we still need available versions to feed solver's search order.
  - Registry entry holds versions newest-first after Step 1; present them to solver as-is.
  - **Note:** This version list comes from the registry cache (registry.dat), which contains metadata for all packages. The solver will later check the package cache for individual elm.json files and download them on miss if online.

### 3.4 Handling Offline Mode
- If `env->offline` and the registry does not contain the requested package, fail fast without attempting HTTP. Error text should indicate offline mode (“Package not found in local cache and offline mode is active.”).
- When offline and package exists, proceed but note in logs that network fetch is skipped; actual package download later (Step 8) must also honor offline status.

### 3.5 Interaction With Later Steps
- Step 3 populates an `InstallPlan` stub:
  ```c
  typedef struct {
      PackageId target;
      PromotionType promotion;
      RegistryEntry *registry_entry;
      bool needs_solver;
  } InstallPlan;
  ```
- If `promotion != PROMOTION_NONE`, Step 5 will call `elm_json_write` immediately and skip solver+download.
- When `needs_solver == true`, pass `registry_entry` and the constraint map to the solver (Step 4 onward).

---

## File Format and Persistence Summary
- `elm.json`: textual JSON, parsed/written using cJSON helpers already in the repo. Always update via temp file rename.
- `registry.dat`: binary format documented in `wrap/doc/file_formats.md`. Use big-endian helpers and fail gracefully when encountering truncated data.
- `all-packages` and `all-packages/since/{count}`: JSON via libcurl downloads, parsed using cJSON. Keep responses in memory buffers since files are small (<2MB).
- Caching directories: follow `CacheConfig` paths (`$ELM_HOME/0.19.1/packages/…`). Ensure directories exist before writing registry or package caches.

---

## Error Reporting Guidelines
- Surface actionable messages with remediation advice (e.g., rerun online, delete corrupted registry).
- Include underlying libcurl errors (use `curl_easy_strerror`).
- For JSON parsing failures, show ~80 chars around the offending location (cJSON’s error pointer) to aid debugging.
- When file operations fail, include path and `strerror(errno)`.

---

## Next Steps After Step 3
- With `InstallPlan` ready, proceed into the solver (Step 4) using the constraint map produced above.
- **Important: During dependency solving, if a package version's metadata (elm.json) is not found in the package cache, only the elm.json file is downloaded for that specific version—not the full package source code.** This metadata fetch allows the solver to check dependencies without downloading unnecessary full packages. Full package downloads occur later during verification (Step 8) for the final resolved set of dependencies.
- Ensure that `InstallEnv` persists for the remainder of the install command so cached registry data and curl session can be reused for fetching package tarballs later in the process.

---

## Happy Paths

All happy paths assume the package exists in the registry and the user provided a valid package name.

### Online

#### Package in Cache
1. Initialize environment and load/update registry from cache/network.
2. Parse elm.json and check for existing package (promote if indirect/test).
3. If not promoted, solver finds compatible versions using cached elm.json files.
4. Verify dependencies: download full packages only if sources missing.
5. Update elm.json and report success.

#### Package Not in Cache
1. Initialize environment and load/update registry from cache/network.
2. Parse elm.json and check for existing package (promote if indirect/test).
3. If not promoted, solver downloads elm.json for tried versions, finds solution.
4. Verify dependencies: download full packages for all in solution.
5. Update elm.json and report success.

### Offline

#### Package in Cache
1. Initialize environment and load registry from cache (no network).
2. Parse elm.json and check for existing package (promote if indirect/test).
3. If not promoted, solver uses cached elm.json to find solution.
4. Verify dependencies using cached packages.
5. Update elm.json and report success.

#### Package Not in Cache
1. Fail with error: package not in cache and offline mode active.
