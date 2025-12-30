# Build Scripts

Scripts for building and testing Debian/Ubuntu packages.

## build-debs.sh

Builds `.deb` packages for multiple distributions and architectures using Docker.

### Usage

```bash
./scripts/build-debs.sh
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `VERSION` | required: either set via environment or present in top-level `VERSION` file — no default | Package version |
| `DOCKER_CMD` | `docker` | Docker command |
| `SKIP_BUILD` | `0` | Set to `1` to skip compilation (use existing binary) |
| `SUITE` | (all) | Build only this suite (e.g., `bookworm`, `trixie`) |
| `ARCH` | (all) | Build only this arch (e.g., `amd64`, `arm64`) |

### Supported Targets

**Debian:**
- bookworm (amd64, arm64)
- trixie (amd64, arm64)

**Ubuntu:**
- jammy (amd64, arm64)
- noble (amd64, arm64)
- plucky (amd64, arm64)
- questing (amd64, arm64)

### How It Works

1. Captures git commit hash at the start to ensure all builds use identical source
2. For each suite/arch combination:
   - Spins up a clean container with the appropriate base image
   - Installs build dependencies
   - Clones the repo from the mounted local directory (no network traffic)
   - Checks out the exact commit
   - Builds with `make rebuild`
   - Detects shared library dependencies using `dpkg-shlibdeps`
   - Packages the binary into a `.deb` file
3. Skips targets where the `.deb` already exists in `dist/`

### Output

Packages are written to `dist/`:
```
dist/elm-wrap_0.5.0_bookworm_amd64.deb
dist/elm-wrap_0.5.0_bookworm_arm64.deb
...
```

## test-debs.sh

Tests `.deb` packages by installing them in clean containers and verifying the binary runs.

### Usage

```bash
./scripts/test-debs.sh
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `VERSION` | `0.5.0` | Package version to test |
| `DOCKER_CMD` | `docker` | Docker command |
| `SUITE` | (all) | Test only this suite (e.g., `bookworm`, `trixie`) |
| `ARCH` | (all) | Test only this arch (e.g., `amd64`, `arm64`) |

### How It Works

1. For each suite/arch combination:
   - Skips if the `.deb` file doesn't exist
   - Spins up a clean container
   - Installs the package with `apt-get install` (resolves dependencies)
   - Runs `wrap version` to verify the installation
2. Reports PASS/FAIL/SKIP for each target
3. Exits with code 1 if any tests failed

### Example Output

```
[test-debs] Testing elm-wrap_0.5.0_bookworm_amd64.deb on debian:bookworm-slim (linux/amd64)...
[test-debs] PASS: elm-wrap_0.5.0_bookworm_amd64.deb -> 0.5.0@main-abc12345-2024-01-15T10:30:00Z
[test-debs] SKIP: elm-wrap_0.5.0_trixie_amd64.deb (not found)
[test-debs] =========================================
[test-debs] Results: 1 passed, 0 failed, 1 skipped
```

## Workflow

Typical workflow for building and testing packages:

```bash
# Build all packages (skips existing ones)
./scripts/build-debs.sh

# Rebuild specific packages by removing them first
rm dist/elm-wrap_0.5.0_bookworm_*.deb
./scripts/build-debs.sh

# Test all built packages
./scripts/test-debs.sh

# Build with a different version
VERSION=0.6.0 ./scripts/build-debs.sh
```

## Requirements

- Docker (or OrbStack on macOS)
- Multi-architecture support (for arm64 builds on amd64 host, or vice versa)
- Git repository with the source code
