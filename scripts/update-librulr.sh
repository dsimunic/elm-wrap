#!/usr/bin/env bash
set -euo pipefail

# update-librulr.sh
# Rebuilds librulr.a for all target platforms and copies to external/lib/
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
info "All requested libraries updated successfully!"
echo ""
info "Library summary:"
find "$EXTERNAL_LIB" -name "librulr.a" -exec sh -c 'echo "  $(dirname "$1" | xargs basename)/librulr.a: $(wc -c < "$1" | tr -d " ") bytes"' _ {} \;

echo ""
info "Next steps:"
echo "  1. Test build:  make clean all"
echo "  2. Commit:      git add external/lib && git commit -m 'Update librulr.a for all platforms'"
