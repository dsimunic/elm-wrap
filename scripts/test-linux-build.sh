#!/usr/bin/env bash
set -euo pipefail

# test-linux-build.sh
# Test the build on Linux using Docker to catch GCC-specific issues
# before pushing to CI.
#
# Usage:
#   ./scripts/test-linux-build.sh         # Full build and test
#   ./scripts/test-linux-build.sh quick   # Just compile (no tests)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# Check Docker
if ! command -v docker &>/dev/null; then
    error "Docker not found. Install Docker or OrbStack."
    exit 1
fi

if ! docker info &>/dev/null; then
    error "Docker daemon not running. Start OrbStack or Docker Desktop."
    exit 1
fi

MODE="${1:-full}"
# Use ubuntu:24.04 to match CI (ubuntu-latest)
# Ubuntu's GCC is stricter about warn_unused_result than Debian
IMAGE="ubuntu:24.04"
PLATFORM="linux/amd64"

info "Testing Linux build (GCC) in Docker..."
info "Image: $IMAGE, Platform: $PLATFORM"
echo ""

# Build command
BUILD_CMD='
set -e
apt-get update -qq
apt-get install -y -qq build-essential libcurl4-openssl-dev rsync libcurl4 >/dev/null 2>&1
echo "=== Cleaning ==="
rm -rf build bin lib
mkdir -p build
echo ""
echo "=== Building ==="
make -j1 all
echo ""
echo "=== Verifying binary ==="
./bin/wrap --version
'

TEST_CMD='
echo ""
echo "=== Running tests ==="
make check
'

if [[ "$MODE" == "quick" ]]; then
    info "Quick mode: build only, no tests"
    FULL_CMD="$BUILD_CMD"
else
    FULL_CMD="$BUILD_CMD$TEST_CMD"
fi

if docker run --rm --platform "$PLATFORM" \
    -v "$PROJECT_ROOT:/src" \
    -w /src \
    "$IMAGE" \
    bash -c "$FULL_CMD"; then
    echo ""
    info "Linux build succeeded!"
else
    echo ""
    error "Linux build failed!"
    exit 1
fi
