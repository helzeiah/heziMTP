#!/usr/bin/env bash
set -euo pipefail

# ── heziMTP build script ──────────────────────────────────────────────────────
# Usage:
#   ./build.sh            → release build (default)
#   ./build.sh debug      → debug build
#   ./build.sh release    → release build
#   ./build.sh clean      → wipe all build dirs
#   ./build.sh run        → release build + open the app
#   ./build.sh run debug  → debug build + open the app

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS=$(sysctl -n hw.logicalcpu)

# ── Parse args ────────────────────────────────────────────────────────────────
MODE="release"
RUN=false

for arg in "$@"; do
    case "$arg" in
        debug)   MODE="debug"   ;;
        release) MODE="release" ;;
        run)     RUN=true       ;;
        clean)
            echo "Cleaning build directories..."
            rm -rf "$SCRIPT_DIR/build" "$SCRIPT_DIR/build_debug" "$SCRIPT_DIR/build_release"
            echo "Done."
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [debug|release|clean|run]"
            exit 1
            ;;
    esac
done

# ── Resolve build dir and CMake config ───────────────────────────────────────
if [[ "$MODE" == "debug" ]]; then
    BUILD_DIR="$SCRIPT_DIR/build_debug"
    CMAKE_TYPE="Debug"
    LABEL="Debug"
else
    BUILD_DIR="$SCRIPT_DIR/build_release"
    CMAKE_TYPE="Release"
    LABEL="Release"
fi

APP_PATH="$BUILD_DIR/heziMTP.app"

# ── Configure (only if CMakeCache.txt is missing) ────────────────────────────
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "── Configuring ($LABEL) ─────────────────────────────────────────"
    cmake -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE="$CMAKE_TYPE" \
          -S "$SCRIPT_DIR"
fi

# ── Build ────────────────────────────────────────────────────────────────────
echo "── Building ($LABEL, $JOBS threads) ────────────────────────────────"
cmake --build "$BUILD_DIR" -j"$JOBS"

# ── Result ───────────────────────────────────────────────────────────────────
BINARY="$APP_PATH/Contents/MacOS/heziMTP"
SIZE=$(du -sh "$BINARY" 2>/dev/null | cut -f1)
ARCH=$(file "$BINARY" | grep -o "arm64\|x86_64" | tr '\n' '+' | sed 's/+$//')
echo ""
echo "── Built: $APP_PATH"
echo "   Binary: $SIZE  ($ARCH universal)"

if $RUN; then
    echo "── Launching..."
    open "$APP_PATH"
fi
