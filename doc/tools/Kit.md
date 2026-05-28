# Kit

Installs a new set of tools and packages on the local machine.

## Synopsis

`wrap kit install PATH [--dry-run] [--yes] [--check-urls]`

- if the path ends in `.kit`, tries to open the specific manifest file and errors out if not found.
- if the path is a directory, reads the path and looks for a `.kit` file. 
- If there's more than one kit file found at precise path, errors out with "A kit must contain a single kitfile." 
- `--yes` (`-y`) skips the confirmation prompt and auto-confirms creating the tool directory.
- `--dry-run` verifies the resolved local kit, reports the tools and packages it would install/register, and exits without prompting or installing anything.
- `--check-urls` with `--dry-run` additionally probes each URL-backed `tool:` with an HTTP HEAD request and fails if any is unreachable.

`wrap kit update <KIT-URL|PATH> [--dry-run] [--yes] [--check-urls]`

Installs, reinstalls, or updates a kit, tracking versions on disk. It accepts a
kit URL or a local path (a `.kit` file or a directory containing one),
fetches/reads and parses the manifest, then acts on the kit named by its
`kit:` key.

### `--dry-run` (verification only)

Non-destructively verifies that a kit is consumable — used by the release
pipeline as a post-publish check. It reports the tools and packages it *would*
register/download and writes nothing to disk or the registry.

- URLs are fetched into memory, bounded by the same size cap as a local kitfile
  read; nothing is downloaded to disk.
- `--check-urls` additionally probes each `tool:` URL with an HTTP HEAD request
  and fails if any is unreachable.
- Exit status is `0` only when the manifest is valid and every `tool:` URL is
  well-formed (and, with `--check-urls`, reachable). Any fetch or parse failure
  exits non-zero with a descriptive message.

### Real run (no `--dry-run`)

Without `--dry-run`, the command shows the plan, prompts for confirmation
(skip with `--yes`), and then performs one of three actions based on the kit's
`kit:` name and `version:`. Installed kit state lives under:

    $WRAP_HOME/kits/<KIT>/<VERSION>/<KIT>.kit
    $WRAP_HOME/kits/<KIT>/current        -> symlink to the active <VERSION> dir

Source resolution mirrors `kit install`: tools download from their `url:`, and
`local-dev:`/`package:` entries come from the on-disk `packages/<elm>/...`
folder next to the kitfile. A bare URL kit (a single `.kit` file) can therefore
only (re)install its listed tools.

- **Install** (no `current` exists): install the kit, store the kitfile, and
  point `current` at the new version.
- **Reinstall** (`current` exists with the same version): the new kitfile must
  match the stored one by SHA-256. If it matches, re-run the install
  (re-download tools, re-register local-dev, re-link). If it differs, error out
  with *"This kitfile is invalid as an update source for version ..."*.
- **Update** (`current` exists with a different version):
  1. download all tools to their versioned `$WRAP_HOME` locations (abort
     untouched if any download fails);
  2. store the new kitfile;
  3. remove the tool symlinks and local-dev registrations listed in the
     *current* kitfile (local-dev removal also deletes the ELM_HOME cache
     symlink);
  4. symlink the new tools and re-register the new local-dev packages, and
     install the new `package:` entries (old versions are left in place);
  5. promote the new version to `current`.

All changes are reported as `kit install` does, and a non-zero exit indicates
one or more failures.

## Structure

A kit is a directory tree potentially containing well-known folders: 

```
.
├── elm-run.kit
├── LICENSE
├── packages
│   └── 0.19.1
├── README.md
└── tools
    ├── elm-run
    └── elm-runc
```

Mandatory files that must be present: 

    * NAME.kit

## Installation

- wrap checks the presence of the `NAME.kit` kitfile. 
  - if not found, error out
  - if multiple found, error out.

- wrap processes each package from the kitfile and installs them

    - `package:` is installed like any other in ELM_HOME

    - `local-dev:` 
        - register as local-dev in-place

    - scans `packages/ELM_VERSION` folder if it exists, and installs all packages found.

    - scans `tool` folder if it exists and installs all tools found.

- for tools listed in the kitfile, a local directory with a LICENSE file must exist; otherwise the tool is ignored.

    - tool is always a download from some url
    - tools downloaded from a url are installed in the $WRAP_HOME/tools/tool-name/KIT_VERSION/bin
    - tools are always executables and are symlinked into the `TOOL_BIN_PATH`.

Installer can only perform three actions: download files, symlink to `TOOL_BIN_PATH` and register packages. There are no 
post-install actions beyond that. 

Installing to the TOOL_BIN_PATH is always via symlinks.

Preferred executable path is `~/.local/bin` (defined as `TOOL_BIN_PATH` macro), overridden with `WRAP_TOOL_BIN_PATH` env var.

If the preferred executable path doesn't exist, wrap will ask the user for permission to create it, after which it will continue installing 
tools.

If the kitfile contains at least one `tool:` key (or valid binary in `tool` path) and the `TOOL_BIN_PATH` exists but wrap has no permission 
to write into it, error out and stop further processing.

## Kitfile

The kitfile manifest follows the format of local-dev package registry.

Unlike the registry manifest, package is required only to have the name listed. The installer will use the last three
directories as the package info: author/name/version and use that for the installation equivalent of `wrap install --local-dev author/name@version.

```
format 2
elm 0.19.1

kit: elm-run
    version: 0.3.0-preview.2026-05-25T083419

tool: elm-runc
    url: https://github.com/elm-run/releases/releases/download/elm-runc-0.19.1/elm-runc

tool: compile
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/compile

tool: disasm
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/disasm

tool: elm-perf
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/elm-perf

tool: host-run
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/host-run

tool: host-run-profiled
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/host-run-profiled

tool: host-worker
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/host-worker

tool: run
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/run

tool: run-c
    url: https://github.com/elm-run/releases/releases/download/0.3.0-preview.2026-05-25T083419/run-c

local-dev: elm-run/basis/1.0.0

local-dev: elm-run/binary/1.0.0

local-dev: elm-run/capabilities/1.0.0

local-dev: elm-run/cli/1.0.0

local-dev: elm-run/fs/1.0.0

local-dev: elm-run/log/1.0.0

local-dev: elm-run/net/1.0.0

local-dev: elm-run/os/1.0.0

local-dev: elm-run/rendezvous/1.0.0

local-dev: elm-run/stdio/1.0.0

local-dev: elm-run/terminal/1.0.0

local-dev: elm-run/terminfo/1.0.0

local-dev: elm-run/worker/1.0.0
```

## Processing the kitfile

Kitfile starts with the same version header as the registry file:

    format 2
    elm 0.19.1

The first entry must be exactly one `kit: NAME` definition. If it isnt't, error out.

A `kit:` entry must have a non-empty version specified.

    kit: elm-run
        version: 0.3.0-preview.2026-05-25T083419

If it doesn't, error out.

If there are multiple `kit:` entries, error out.

The entries following can be one of:

    tool:
    package:
    local-dev:

If the kitfile contains any `tool:` entries, the preferred executable folder must exist. Wrap must check
this before it starts downloading tools. If the folder doesn't exist, it should request creation from the
user.

### tools/ folder

If the tools folder exists, scan it for directories of the form `NAME/VERSION/bin/NAME`. If a non-empty FILE 
exists, check if the directory contains a non-empty LICENSE file at `NAME/VERSION/LICENSE`.

If so, add the file to the list of files to symlink in-place. A `tool:` specification of the same NAME
overrides the discovered binary and ignores all versions.

### tool:

The tool entry must have a non-empty name, and it must be a valid file name.

The only property is `url:`, a url to the binary.

    tool: elm-runc
        url: https://github.com/elm-run/releases/releases/download/elm-runc-0.19.1/elm-runc

wrap downloads the file from the specified URL and atomically renames to the name specified in `tool:`,
then symlinks to the preferred executable folder.

### packages/ELM_VERSION

If a folder `packages/ELM_VERSION` exists relative to the kitfile's parent directory, list all directories in the form
`author/name/version` that contain a valid package elm.json an install them as if `wrap package install --from-path PATH` was
specified.

A `package:` key overrides any package of the same version.

ELM_VERSION is the string after `elm ` entry on the second row of the kitfile.

### package:

    package: author/name[@VERSION]

If the package is specified with one line, check if the package name is a valid name and exists at the path `packages/ELM_VERSION/AUTHOR/NAME/VERSION`.
If it does, install as if `wrap package install --from-path` was called. If VERSION is not specified, use the first folder within
`packages/ELN_VERSION/AUTHOR/NAME`; that folder must contain a valid `elm.json`. Otherwise write a warning and proceed to the
next package in the list.

The path is relative to the kitfile's own parent directory.

If the folder doesn't exist, install it from the package registry as if `wrap package install PACKAGE[@VERSION]`
was called. Print a warning on error and continue.

### local-dev:

    local-dev: PACKAGE@VERSION

The name must be a valid package name, and the path PACKAGE/VERSION must exist at `packages/ELM_VERSION`. 

If the path exists and contains a valid package `elm.json` register it in local-dev registry as if `wrap package install --local-dev PATH` was 
specified.

### Validation

If the proper format header is missing, error out.

If any other key except those listed is present in the kitfile, refuse to process it.

If an empty `kit:`, `local-dev:`, `package:` or `tool:` entry is specified, error out and refuse to process the kitfile.

