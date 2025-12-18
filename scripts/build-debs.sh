#!/usr/bin/env bash
set -euo pipefail

# Build .deb packages for multiple suites and architectures using Docker
# Usage: ./scripts/build-debs.sh
# Environment overrides:
#   VERSION (default: 0.5.0)
#   DOCKER_CMD (default: docker)
#   SKIP_BUILD (if set to 1, will not run `make clean all` / `make dist`)
#   SUITE (if set, only build for this suite, e.g. "bookworm")
#   ARCH (if set, only build for this arch, e.g. "amd64")

VERSION=${VERSION:-0.5.0}
DOCKER_CMD=${DOCKER_CMD:-docker}
SKIP_BUILD=${SKIP_BUILD:-0}
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

OUTDIR=$(pwd)/dist
mkdir -p "$OUTDIR"

log() { echo "[build-debs] $*"; }

# Capture git info once so all builds have the same commit/timestamp
GIT_COMMIT=$(git rev-parse HEAD)
GIT_COMMIT_SHORT=$(git rev-parse --short=8 HEAD)
GIT_BRANCH=$(git symbolic-ref --short -q HEAD 2>/dev/null || echo "detached")
log "Building from commit $GIT_COMMIT_SHORT ($GIT_BRANCH)"

# Verify docker is available (OrbStack provides this)
if ! command -v "$DOCKER_CMD" >/dev/null 2>&1; then
  echo "ERROR: docker not found in PATH" >&2
  echo "Hint: OrbStack provides docker. Install OrbStack or set DOCKER_CMD." >&2
  exit 1
fi

log "Using docker for container execution"

# Select an appropriate image for a suite (debian for bookworm/trixie, ubuntu for others)
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

# Packages required to build (full set from README); if installing fails we try minimal set
# Note: libssh-dev AND libssh2-1-dev needed because curl uses different SSH backends on different distros
FULL_DEPS=(build-essential git curl ca-certificates pkg-config \
  libcurl4-openssl-dev libnghttp2-dev libidn2-dev libunistring-dev libgpg-error-dev libgcrypt20-dev \
  libssl-dev libldap2-dev libbrotli-dev librtmp-dev libssh-dev libssh2-1-dev libpsl-dev libkrb5-dev libzstd-dev zlib1g-dev rsync dpkg-dev fakeroot)
MINIMAL_DEPS=(build-essential curl ca-certificates rsync dpkg-dev fakeroot)

# Join arrays into strings for in-container use
PACKAGES_FULL_STR="${FULL_DEPS[*]}"
PACKAGES_MIN_STR="${MINIMAL_DEPS[*]}"

for suite in "${SUITES[@]}"; do
  image=$(select_image "$suite") || image=""
  if [ -z "$image" ]; then
    log "Warning: could not find a Docker image for suite '$suite'; skipping all arches for this suite"
    continue
  fi

  for arch in "${ARCHES[@]}"; do
    outname="elm-wrap_${VERSION}_${suite}_${arch}.deb"
    outfile="$OUTDIR/$outname"

    if [ -f "$outfile" ]; then
      log "Already exists: $outfile (skipping)"
      continue
    fi

    platform="linux/$arch"
    log "Building for suite=$suite arch=$arch image=$image platform=$platform -> $outname"

    # Build commands inside container (expand package lists now, not in container)
    container_cmds=$(cat <<EOF
set -euo pipefail

# Ensure defaults inside container
SKIP_BUILD=\${SKIP_BUILD:-0}
ARCH=\${ARCH:-$arch}
VERSION=\${VERSION:-$VERSION}
GIT_COMMIT=\${GIT_COMMIT:-}

# Install deps (try full set, fallback to minimal)
apt-get update -qq 2>/dev/null
if ! apt-get install -qq -y --no-install-recommends $PACKAGES_FULL_STR 2>/dev/null; then
  echo "Full deps install failed; trying minimal set"
  apt-get -qq install -y --no-install-recommends $PACKAGES_MIN_STR
fi

# Clone from mounted repo to get proper git metadata
BUILDDIR=/tmp/build-src
rm -rf "\$BUILDDIR"
echo "Cloning from /work to \$BUILDDIR..."
if ! git clone --no-local /work "\$BUILDDIR"; then
  echo "ERROR: git clone failed"
  exit 1
fi
cd "\$BUILDDIR"
echo "Checking out \$GIT_COMMIT..."
if ! git checkout "\$GIT_COMMIT"; then
  echo "ERROR: git checkout failed"
  exit 1
fi

# Build from cloned tree (capture output, show only on error)
if [ "\$SKIP_BUILD" != "1" ]; then
  echo "Building..."
  if ! make -s clean all > /tmp/make.log 2>&1; then
    echo "=== make failed (last 15 lines) ==="
    tail -15 /tmp/make.log
    exit 1
  fi
fi

# Prepare staging (binary is in \$BUILDDIR/bin/wrap from the cloned build)
STAGING=/tmp/pkg-stage
rm -rf "\$STAGING"
mkdir -p "\$STAGING/usr/bin"
install -m 0755 "\$BUILDDIR/bin/wrap" "\$STAGING/usr/bin/wrap"

# Create DEBIAN control
mkdir -p "\$STAGING/DEBIAN"

# Calculate shared library dependencies using dpkg-shlibdeps
# It needs a debian/control file to work, so we create a temporary one
mkdir -p /tmp/deb-src/debian
cat > /tmp/deb-src/debian/control <<DEBSRC
Source: elm-wrap
Build-Depends: debhelper

Package: elm-wrap
Architecture: any
Description: elm-wrap
DEBSRC

# Run dpkg-shlibdeps from the fake source dir
DEPS=\$(cd /tmp/deb-src && dpkg-shlibdeps -O "\$STAGING/usr/bin/wrap" 2>/dev/null | sed 's/^shlibs:Depends=//')

# Fallback if dpkg-shlibdeps fails or returns empty
if [ -z "\$DEPS" ]; then
  echo "Warning: dpkg-shlibdeps returned empty, using ldd fallback"
  # Extract library deps from ldd and find packages
  DEPS=\$(ldd "\$STAGING/usr/bin/wrap" 2>/dev/null | awk '/=>/{print \$3}' | xargs -r dpkg -S 2>/dev/null | cut -d: -f1 | sort -u | paste -sd, - || echo "libc6, libcurl4")
fi

cat > "\$STAGING/DEBIAN/control" <<CONTROL
Package: elm-wrap
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${arch}
Depends: \$DEPS
Maintainer: Damir Simunic <packages@oomm.dev>
Description: elm-wrap - wrapper for Elm package management (built from source)
CONTROL

rm -rf /tmp/deb-src

# Set permissions
chmod -R 0755 "\$STAGING/usr/bin"

# Build .deb with fakeroot to get ownership right
mkdir -p /work/dist
fakeroot dpkg-deb --build "\$STAGING" "/work/dist/elm-wrap_${VERSION}_${suite}_${arch}.deb"

# Clean
rm -rf "\$STAGING"
EOF
)

    # Run the container with volume mount to the repo (current dir)
    $DOCKER_CMD run --rm --platform="$platform" -e SKIP_BUILD="$SKIP_BUILD" -e VERSION="$VERSION" -e ARCH="$arch" -e GIT_COMMIT="$GIT_COMMIT" -v "$(pwd)":/work -w /work "$image" bash -lc "$container_cmds" || {
      log "Build failed for $suite/$arch (image $image). See output above."
      continue
    }

    if [ -f "$outfile" ]; then
      log "SUCCESS: $outfile"
    else
      log "FAIL: expected $outfile but it was not created"
    fi
  done
done

log "All done. .deb files (if created) are in: $OUTDIR"

# End of script
