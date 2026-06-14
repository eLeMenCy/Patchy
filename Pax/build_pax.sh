#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build_pax.sh — Build all Patchy Pax in one go
#
#  Usage:
#    ./build_pax.sh              # Build only
#    ./build_pax.sh --install    # Build + install to Patchy Pax folder
#    ./build_pax.sh --clean      # Clean build directory first, then build
#    ./build_pax.sh --clean --install
#
#  Built binaries land in:  Pax/build/pax/
#  Install destination:     ~/Library/Patchy/Pax/   (macOS)
#                           ~/.patchy/pax/           (Linux)
# ─────────────────────────────────────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL=false
CLEAN=false

# ── Parse arguments ───────────────────────────────────────────────────────────
for arg in "$@"; do
    case $arg in
        --install) INSTALL=true ;;
        --clean)   CLEAN=true ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [--clean] [--install]"
            exit 1
            ;;
    esac
done

# ── Clean ─────────────────────────────────────────────────────────────────────
if [ "$CLEAN" = true ]; then
    echo "→ Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# ── Configure ─────────────────────────────────────────────────────────────────
echo "→ Configuring..."
cmake -S "$SCRIPT_DIR" \
      -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      --log-level=WARNING

# ── Build ─────────────────────────────────────────────────────────────────────
echo "→ Building..."
cmake --build "$BUILD_DIR" \
      --config Release \
      --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

# ── Report ────────────────────────────────────────────────────────────────────
echo ""
echo "✓ Built Pax:"
find "$BUILD_DIR/pax" -name "*.dylib" -o -name "*.so" 2>/dev/null | sort | while read -r f; do
    echo "    $(basename "$f")"
done

# ── Install ───────────────────────────────────────────────────────────────────
if [ "$INSTALL" = true ]; then
    echo ""
    echo "→ Installing..."
    cmake --install "$BUILD_DIR" --prefix "$HOME"
    if [ "$(uname)" = "Darwin" ]; then
        DEST="$HOME/Library/Patchy/Pax"
    else
        DEST="$HOME/.patchy/pax"
    fi
    echo ""
    echo "✓ Pax installed to:"
    echo "    $DEST"
fi

echo ""
echo "Done."
