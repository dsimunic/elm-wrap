# Building elm-wrap

This document covers building elm-wrap from source, including the librulr dependency and multi-architecture builds.

## Quick Start

```bash
make clean all    # Build everything
make check        # Run tests
./bin/wrap --version
```

## Prerequisites

- **macOS**: Xcode Command Line Tools (`xcode-select --install`)
- **Linux**: `build-essential`, `libcurl4-openssl-dev` and related curl dependencies
- **Docker/OrbStack**: Required only for updating Linux libraries on macOS

## Build Targets

| Target | Description |
|--------|-------------|
| `make all` | Build wrap binary and tools |
| `make clean` | Remove build artifacts |
| `make rebuild` | Clean, build, and install to ~/.local/bin |
| `make check` | Run test suite |
| `make dist` | Create source distribution tarball |
| `make distcheck` | Build and test from tarball |

## Architecture Support

elm-wrap builds for multiple platforms:

| Platform | Architecture | CI Runner |
|----------|--------------|-----------|
| macOS | arm64 (Apple Silicon) | macos-14 |
| macOS | x86_64 (Intel) | macos-15-intel |
| Linux | x86_64 | ubuntu-latest |
| Linux | arm64 | (deb builds only) |

The Makefile auto-detects the current platform and selects the appropriate pre-built library.

## librulr.a Dependency

elm-wrap depends on `librulr.a`, a static library from the [Rulr](../Rulr) project (a Datalog engine). The library is pre-built and committed to the repository to avoid build-time dependencies.

### Library Locations

```
external/lib/
├── darwin-arm64/librulr.a    # macOS Apple Silicon
├── darwin-x86_64/librulr.a   # macOS Intel
├── linux-x86_64/librulr.a    # Linux amd64
└── linux-arm64/librulr.a     # Linux arm64
```

### Why Pre-built Libraries?

1. **No LTO (Link-Time Optimization)**: Libraries are built without `-flto` to avoid LLVM bitcode version mismatches across different compiler versions
2. **Portability**: One library per architecture works across all OS versions (e.g., all Debian/Ubuntu releases use the same `linux-x86_64` library)
3. **Simplicity**: CI and deb builds don't need access to the Rulr source

### Updating librulr.a

When the Rulr project changes, rebuild and update the libraries:

```bash
# Update all libraries (requires Docker for Linux builds)
./scripts/update-librulr.sh

# Or update specific platforms:
./scripts/update-librulr.sh darwin   # macOS only (no Docker needed)
./scripts/update-librulr.sh linux    # Linux only (requires Docker)
```

The script:
1. Builds libraries in the Rulr project (../Rulr by default, or set `RULR_ROOT`)
2. Copies them to `external/lib/<platform>/`
3. Reports sizes and next steps

### Manual Library Build

If you need to build libraries manually:

**macOS (from the Rulr project):**
```bash
cd /path/to/Rulr
make libs-darwin
# Creates: lib/darwin-arm64/librulr.a, lib/darwin-x86_64/librulr.a
```

**Linux (via Docker on macOS):**
```bash
cd /path/to/Rulr

# x86_64
docker run --rm --platform linux/amd64 -v "$PWD:/src" -w /src debian:bookworm \
  bash -c 'apt-get update -qq && apt-get install -y -qq build-essential && make lib-linux-x86_64'

# arm64
docker run --rm --platform linux/arm64 -v "$PWD:/src" -w /src debian:bookworm \
  bash -c 'apt-get update -qq && apt-get install -y -qq build-essential && make lib-linux-arm64'
```

**Linux (native):**
```bash
cd /path/to/Rulr
make lib-host  # Builds for current architecture
```

## Debian/Ubuntu Packages

See [scripts/README.md](../scripts/README.md) for building `.deb` packages.

The deb build process:
1. Uses Docker to build in clean Debian/Ubuntu containers
2. Builds for 6 distros × 2 architectures = 12 packages
3. All distros use the same `linux-x86_64` or `linux-arm64` library

```bash
./scripts/build-debs.sh     # Build all packages
./scripts/test-debs.sh      # Test packages in containers
```

## CI/CD

GitHub Actions workflows:

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| `build.yml` | Push/PR | Build and test on macOS + Linux |
| `release.yml` | Tag `v*` | Create GitHub release with binaries |
| `trigger-tap-update.yml` | Tag `v*` | Update Homebrew tap |

See [RELEASING.md](RELEASING.md) for the full release process.

## Troubleshooting

### LTO/Bitcode Errors

```
ld: Invalid bitcode version (Producer: '1700.x' Reader: '1500.x')
```

The pre-built library was compiled with a newer LLVM than the build system. Solution: rebuild the library without LTO using `./scripts/update-librulr.sh`.

### Missing Library

```
No rule to make target 'external/lib/darwin-arm64/librulr.a'
```

The library for your platform is missing. Run:
```bash
./scripts/update-librulr.sh
```

### Architecture Mismatch

The Makefile auto-detects architecture. To verify:
```bash
make -p | grep LIB_PLATFORM
# Should show: LIB_PLATFORM = darwin-arm64 (or your platform)
```
