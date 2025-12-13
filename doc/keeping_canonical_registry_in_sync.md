# Keeping the canonical registry in sync (V1 `registry.dat`)

## Overview

In V1 mode, both `elm` and `wrap` rely on `$ELM_HOME/<elm-version>/packages/registry.dat` as a local cache of the canonical package registry.

The canonical registry supports incremental updates via `/all-packages/since/<N>`, where `N` is the canonical “number of published versions seen so far” counter (often called `since_count`).

**elm-wrap** supports local development packages (`wrap install --local-dev`) without breaking this protocol by treating the V1 header counter as canonical-only: local-only versions may be present in the registry map, but they do not inflate the `since_count` stored in the V1 file header.

## Background: what `registry.dat`’s header means

The canonical registry exposes:

- `GET /all-packages` → full snapshot (`{ "author/name": ["1.0.0", ...], ... }`)
- `GET /all-packages/since/<N>` → incremental “new versions since count N” (`["author/name@1.2.3", ...]`)

The `since/<N>` protocol depends on `N` being **the canonical count of published versions**, not “the number of versions in my local file”.

If local tooling invents extra versions and increments `N`, the client can “skip forward” and miss upstream publications.

## Implementation in **elm-wrap** (V1)

### On-disk format: where the two header numbers live

`wrap` implements the V1 cache reader/writer in:

- `src/registry.c` (`registry_load_from_dat()`, `registry_dat_write()`)
- `src/registry.h` (`Registry`, `RegistryEntry`)

The file header is:

1. `u64` big-endian: `Registry.since_count` (canonical `since_count`)
2. `u64` big-endian: `Registry.entry_count` (number of packages)

This matches Elm’s Haskell `Data.Binary` structure: the first number is the “known versions” count, and the second is the map size.

### How `wrap` uses `since_count`

In V1 mode, `wrap` loads the cached registry in:

- `src/install_env.c:install_env_init()`

Key behavior:

- `env->registry = registry_load_from_dat(env->cache->registry_path, &env->known_version_count);`
  - `env->known_version_count` is populated from the header’s `since_count`.
- When online, `wrap` asks for updates using:
  - `GET <registry_url>/all-packages/since/<env->known_version_count>`
  - Implemented in `src/install_env.c:install_env_update_registry()`

`wrap` advances the canonical counter based on the server protocol (see “Canonical sync rules” below), so local-only versions do not cause `wrap` or `elm` to miss upstream updates.

### How local-dev interacts with the V1 registry cache

Local-dev is implemented in:

- `src/commands/package/install_cmd.c` (`--local-dev`, `--from-path`)
- `src/commands/package/install_local_dev.c` (core local-dev mechanics)

When `wrap install --local-dev ...` runs, it calls:

- `register_in_local_dev_registry()` → always updates the local-dev tracking registry file, and also updates V1 `registry.dat`
  - `register_local_dev_v2_text_registry()` writes `WRAP_HOME/<LOCAL_DEV_TRACKING_DIR>/registry-local-dev.dat` (text format)
  - `ensure_local_dev_in_registry_dat()` loads `$ELM_HOME/.../packages/registry.dat`, then inserts into the map using:
    - `registry_add_version_ex(..., count_for_since=false, ...)`
    - which adds a missing version without incrementing `Registry.since_count`
    - and then writes `registry.dat` only when a new version is actually inserted

This keeps local-dev versions discoverable while keeping the canonical header counter correct for `/since/<N>`.

### Local-dev removal and cleanup

Commands that affect local-dev tracking today:

- Adds local-dev packages:
  - `wrap install --local-dev [--from-path PATH] [PACKAGE]`
  - `wrap package install --local-dev ...` (same command group)
  - Includes “register-only mode” when run from a package directory without `--from-path`.
- Removes local-dev tracking and also removes the local-only version from the V1 registry map:
  - `wrap uninstall --local-dev` (or `wrap package uninstall --local-dev`)
    - Calls `unregister_local_dev_package()` which deletes tracking directories under `WRAP_HOME/<LOCAL_DEV_TRACKING_DIR>/packages/...`
    - Removes the corresponding version from `$ELM_HOME/.../packages/registry.dat` (without decrementing `since_count`)
  - `wrap repository local-dev clear ...`
    - Deletes tracking paths, or the whole tracking directory (`clear --all`)
    - Removes corresponding versions from `$ELM_HOME/.../packages/registry.dat` (without decrementing `since_count`)

`wrap uninstall --local-dev` removes local-dev tracking and reverts the local-only V1 registry cache mutation. Regular “uninstall” (`wrap package remove` / `wrap package uninstall`) updates `elm.json` only.

### Other registry mutators

These commands can also desync `elm` if used to add local-only versions:

- `wrap debug registry_v1 add AUTHOR/NAME@VERSION`
- `wrap debug registry_v1 remove AUTHOR/NAME@VERSION`

They intentionally modify `registry.dat` and increment/decrement `Registry.since_count`.

## Previous failure mode (what this prevents)

If local-only tooling increments the header counter, the client can “skip forward” and miss upstream updates:

1. Canonical registry has `since_count = C`.
2. A local-only version is inserted and the header is (incorrectly) written as `since_count = C + 1`.
3. Later, canonical publishes new versions, moving its real count to `C + k`.
4. `elm` asks `/all-packages/since/(C + 1)` and receives only `(k - 1)` items, permanently skipping one upstream publication.

This is exactly the known issue described in `doc/REVIEWERS_GUIDE_0.5.0-preview.2.md`.

## Canonical sync rules

`wrap` keeps the V1 `since_count` aligned with the server protocol, even when local-only versions exist in the map.

### Full snapshot (`GET /all-packages`)

When `wrap` parses a full snapshot:

- It inserts all versions into the registry map without incrementing `Registry.since_count`.
- It sets `Registry.since_count` to the total number of version strings in the JSON payload (sum of all per-package arrays).

This ensures the header matches the canonical “published versions seen” count, regardless of any extra local-only versions in the map.

### Incremental updates (`GET /all-packages/since/<N>`)

When `wrap` parses a `/since` response:

- It inserts each `author/name@version` into the registry map without incrementing `Registry.since_count`.
- It increments `Registry.since_count` by the length of the `/since` response array, even if some entries are already present locally.

This matches the canonical semantics of `/since/<N>` (“events since N”), and avoids falling behind when a local-only version later appears in the canonical stream.

## Sidecar canonical count (`registry.dat.since-count`)

To repair a previously-inflated header counter without deleting cache files, `wrap` persists the canonical `since_count` alongside `registry.dat`:

- Path: `$ELM_HOME/<elm-version>/packages/registry.dat.since-count`
- Format: a single integer followed by `\n`

`wrap` writes the sidecar when it successfully syncs from canonical (`/all-packages` or `/since`) and writes the updated `registry.dat`.

On startup in V1 mode, if the sidecar exists and the `registry.dat` header differs, `wrap` rewrites the header back to the sidecar value (best-effort) and uses that value for subsequent `/since/<N>` requests.

## Map size vs canonical counter

- `Registry.since_count` is the canonical `/since/<N>` counter stored in the `registry.dat` header.
- The registry map can contain additional local-only versions; `registry_versions_in_map_count()` reports the actual “versions in map” count.

Local-dev insertion and local-dev merges may increase the map count without changing `Registry.since_count`.

## Debugging helpers

- `wrap debug registry_v1 list` prints both the header `Since count` and `Versions in map`.
- `wrap debug registry_v1 apply-since <json>` applies an offline `/since` JSON array (useful for testing the “advance by response length” rule).
