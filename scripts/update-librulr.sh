#!/usr/bin/env bash
set -euo pipefail

# update-librulr.sh
# Rebuilds librulr.a for all target platforms and copies to external/lib/
# Also syncs exported headers to external/include/rulr/ (from Rulr's check export).
#
# Prerequisites:
#   - Rulr project checked out at ../Rulr (relative to elm-wrap)
#   - Docker/OrbStack running (for Linux builds)
#
# Usage:
#   ./scripts/update-librulr.sh          # Build all platforms
#   ./scripts/update-librulr.sh darwin   # macOS only (no Docker needed)
#   ./scripts/update-librulr.sh linux    # Linux only (requires Docker)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELM_WRAP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RULR_ROOT="${RULR_ROOT:-$ELM_WRAP_ROOT/../Rulr}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# Check Rulr project exists
if [[ ! -d "$RULR_ROOT" ]]; then
    error "Rulr project not found at: $RULR_ROOT"
    error "Set RULR_ROOT environment variable or ensure ../Rulr exists"
    exit 1
fi

RULR_ROOT="$(cd "$RULR_ROOT" && pwd)"
info "Using Rulr project at: $RULR_ROOT"

# Target directories
EXTERNAL_LIB="$ELM_WRAP_ROOT/external/lib"
mkdir -p "$EXTERNAL_LIB"/{darwin-arm64,darwin-x86_64,linux-x86_64,linux-arm64}

sync_headers() {
    local rulr_export="$RULR_ROOT/check/external/include"
    local external_include="$ELM_WRAP_ROOT/external/include/rulr"

    if [[ ! -d "$rulr_export" ]]; then
        warn "Rulr exported headers not found at: $rulr_export"
        warn "Headers not updated."
        return 0
    fi

    info "Syncing Rulr exported headers to elm-wrap..."
    mkdir -p "$external_include"

    # Copy without deleting: elm-wrap may carry additional integration headers
    # (e.g. compiled .dlc helpers) that aren't part of Rulr's exported include set.
    cp -R "$rulr_export"/* "$external_include/"
}

recompile_builtin_rules() {
    local rulrc="$RULR_ROOT/bin/rulrc"
    local src_dir="$ELM_WRAP_ROOT/rulr/rules/src"
    local out_dir="$ELM_WRAP_ROOT/rulr/rules/compiled"

    if [[ ! -d "$src_dir" ]]; then
        warn "Built-in rules source directory not found at: $src_dir"
        warn "Skipping built-in rule compilation."
        return 0
    fi

    if [[ ! -x "$rulrc" ]]; then
        info "Building rulrc compiler..."
        (cd "$RULR_ROOT" && make bin/rulrc)
    fi

    if [[ ! -x "$rulrc" ]]; then
        warn "rulrc not found or not executable at: $rulrc"
        warn "Skipping built-in rule compilation. Built-in .dlc files may be incompatible with the updated library."
        return 0
    fi

    mkdir -p "$out_dir"

    shopt -s nullglob
    local dl_files=("$src_dir"/*.dl)
    shopt -u nullglob

    if [[ ${#dl_files[@]} -eq 0 ]]; then
        warn "No .dl files found in: $src_dir"
        warn "Skipping built-in rule compilation."
        return 0
    fi

    info "Recompiling built-in rules with rulrc..."
    for dl in "${dl_files[@]}"; do
        local base
        base="$(basename "$dl" .dl)"
        "$rulrc" compile "$dl" -o "$out_dir/$base.dlc"
    done
}

build_darwin() {
    info "Building macOS libraries..."
    cd "$RULR_ROOT"
    make libs-darwin

    info "Copying macOS libraries to elm-wrap..."
    cp lib/darwin-arm64/librulr.a "$EXTERNAL_LIB/darwin-arm64/"
    cp lib/darwin-x86_64/librulr.a "$EXTERNAL_LIB/darwin-x86_64/"

    info "macOS libraries updated:"
    ls -la "$EXTERNAL_LIB"/darwin-*/librulr.a
}

build_linux() {
    # Check Docker is available
    if ! command -v docker &>/dev/null; then
        error "Docker not found. Install Docker or OrbStack to build Linux libraries."
        exit 1
    fi

    if ! docker info &>/dev/null; then
        error "Docker daemon not running. Start OrbStack or Docker Desktop."
        exit 1
    fi

    info "Building Linux x86_64 library in Docker..."
    cd "$RULR_ROOT"
    docker run --rm --platform linux/amd64 \
        -v "$RULR_ROOT:/src" -w /src \
        debian:bookworm \
        bash -c 'apt-get update -qq && apt-get install -y -qq build-essential >/dev/null && make lib-linux-x86_64'

    info "Building Linux arm64 library in Docker..."
    docker run --rm --platform linux/arm64 \
        -v "$RULR_ROOT:/src" -w /src \
        debian:bookworm \
        bash -c 'apt-get update -qq && apt-get install -y -qq build-essential >/dev/null && make lib-linux-arm64'

    info "Copying Linux libraries to elm-wrap..."
    cp lib/linux-x86_64/librulr.a "$EXTERNAL_LIB/linux-x86_64/"
    cp lib/linux-arm64/librulr.a "$EXTERNAL_LIB/linux-arm64/"

    info "Linux libraries updated:"
    ls -la "$EXTERNAL_LIB"/linux-*/librulr.a
}

# Parse arguments
TARGET="${1:-all}"

case "$TARGET" in
    darwin)
        build_darwin
        ;;
    linux)
        build_linux
        ;;
    all)
        build_darwin
        echo ""
        build_linux
        ;;
    *)
        error "Unknown target: $TARGET"
        echo "Usage: $0 [darwin|linux|all]"
        exit 1
        ;;
esac

echo ""
sync_headers

echo ""
recompile_builtin_rules

echo ""
info "All requested libraries updated successfully!"
echo ""
info "Library summary:"
find "$EXTERNAL_LIB" -name "librulr.a" -exec sh -c 'echo "  $(dirname "$1" | xargs basename)/librulr.a: $(wc -c < "$1" | tr -d " ") bytes"' _ {} \;

echo ""
info "Next steps:"
echo "  1. Test build:  make clean all"
echo "  2. (Optional) Commit: git add external/lib external/include/rulr"
