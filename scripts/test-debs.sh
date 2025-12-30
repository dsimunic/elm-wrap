#!/usr/bin/env bash
set -euo pipefail

# Test .deb packages by installing them in clean containers and running `wrap version`
# Usage: ./scripts/test-debs.sh
# Environment overrides:
#   VERSION (required: set via environment OR provide a top-level `VERSION` file; no default)
#   DOCKER_CMD (default: docker)
#   SUITE (if set, only test for this suite, e.g. "bookworm")
#   ARCH (if set, only test for this arch, e.g. "amd64")

# Require VERSION: prefer environment override; otherwise read from top-level `VERSION` file; fail if neither exists
if [ -z "${VERSION:-}" ]; then
  if [ -f VERSION ]; then
    VERSION=$(<VERSION)
  else
    echo "ERROR: VERSION not set and no top-level 'VERSION' file found" >&2
    echo "Set the VERSION environment variable or create a top-level 'VERSION' file containing the desired version." >&2
    exit 1
  fi
fi
DOCKER_CMD=${DOCKER_CMD:-docker}
FILTER_SUITE=${SUITE:-}
FILTER_ARCH=${ARCH:-}

ALL_SUITES=("bookworm" "trixie" "jammy" "noble" "plucky" "questing")
ALL_ARCHES=("amd64" "arm64")

# Apply filters
if [ -n "$FILTER_SUITE" ]; then
  SUITES=("$FILTER_SUITE")
else
  SUITES=("${ALL_SUITES[@]}")
fi

if [ -n "$FILTER_ARCH" ]; then
  ARCHES=("$FILTER_ARCH")
else
  ARCHES=("${ALL_ARCHES[@]}")
fi

DISTDIR=$(pwd)/dist

log() { echo "[test-debs] $*"; }

# Verify docker is available
if ! command -v "$DOCKER_CMD" >/dev/null 2>&1; then
  echo "ERROR: docker not found in PATH" >&2
  exit 1
fi

# Select an appropriate image for a suite
select_image() {
  local suite=$1

  case "$suite" in
    bookworm|trixie)
      echo "debian:${suite}-slim"
      ;;
    jammy|noble|plucky|questing)
      echo "ubuntu:${suite}"
      ;;
    *)
      echo ""
      return 1
      ;;
  esac
}

passed=0
failed=0
skipped=0

for suite in "${SUITES[@]}"; do
  image=$(select_image "$suite") || image=""
  if [ -z "$image" ]; then
    log "Warning: no Docker image for suite '$suite'; skipping"
    continue
  fi

  for arch in "${ARCHES[@]}"; do
    debfile="elm-wrap_${VERSION}_${suite}_${arch}.deb"
    debpath="$DISTDIR/$debfile"

    if [ ! -f "$debpath" ]; then
      log "SKIP: $debfile (not found)"
      skipped=$((skipped + 1))
      continue
    fi

    platform="linux/$arch"
    log "Testing $debfile on $image ($platform)..."

    # Run test in container (use apt install ./file.deb to resolve dependencies)
    if output=$($DOCKER_CMD run --rm --platform="$platform" \
      -v "$DISTDIR":/debs:ro \
      "$image" \
      bash -c "apt-get -qq update && apt-get -qq install -y /debs/$debfile && wrap version" 2>&1); then
      log "PASS: $debfile -> $output"
      passed=$((passed + 1))
    else
      log "FAIL: $debfile"
      log "  Output: $output"
      failed=$((failed + 1))
    fi
  done
done

log "========================================="
log "Results: $passed passed, $failed failed, $skipped skipped"

if [ "$failed" -gt 0 ]; then
  exit 1
fi
