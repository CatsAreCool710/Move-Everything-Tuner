#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

MODULE_ID="tuner"

# Parse --debug flag
DEBUG_FLAGS=""
if [ "${1:-}" = "--debug" ]; then
    DEBUG_FLAGS="-DTUNER_DEBUG"
    echo "=== Building Tuner (DEBUG) ==="
else
    echo "=== Building Tuner ==="
fi

# Auto-Docker if no cross prefix set and not already in Docker
if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f /.dockerenv ]; then
    echo "==> No CROSS_PREFIX set, building via Docker..."
    docker build -t tuner-builder -f "$SCRIPT_DIR/Dockerfile" "$ROOT_DIR"
    MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT_DIR:/build" -w /build tuner-builder ./scripts/build.sh ${1:-}
    exit $?
fi

CC="${CROSS_PREFIX:-}gcc"

echo "==> Cross-compiling DSP plugin with $CC..."

SRC_DIR="$ROOT_DIR/src/dsp"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$ROOT_DIR/dist/$MODULE_ID"

mkdir -p "$BUILD_DIR" "$DIST_DIR"

$CC -std=c11 -O3 -g -shared -fPIC \
    -mcpu=cortex-a72 -ffast-math -flto \
    $DEBUG_FLAGS \
    "$SRC_DIR/tuner_plugin.c" \
    "$SRC_DIR/tuner_engine.c" \
    "$SRC_DIR/tuner_audio.c" \
    -o "$BUILD_DIR/dsp.so" \
    -I"$ROOT_DIR/src" \
    -lm

echo "==> Packaging module..."

# Use cat instead of cp to avoid ExtFS deallocation issues with Docker volumes
cat "$BUILD_DIR/dsp.so"         > "$DIST_DIR/dsp.so"
cat "$ROOT_DIR/src/module.json" > "$DIST_DIR/module.json"
cat "$ROOT_DIR/src/help.json"   > "$DIST_DIR/help.json"

# Strip /*DEBUG*/ lines from ui.js in release builds
if [ -n "$DEBUG_FLAGS" ]; then
    cat "$ROOT_DIR/src/ui.js" > "$DIST_DIR/ui.js"
else
    grep -v '/\*DEBUG\*/' "$ROOT_DIR/src/ui.js" > "$DIST_DIR/ui.js"
fi

echo "==> Creating tarball..."
cd "$ROOT_DIR/dist"
tar -czvf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd "$ROOT_DIR"

echo "==> Done: dist/$MODULE_ID-module.tar.gz"
