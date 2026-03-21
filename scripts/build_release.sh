#!/usr/bin/env bash
# Build all hardware variants and collect firmware binaries for release.
# Usage: ./scripts/build_release.sh [version_tag]
# Output: release/ directory with the 3 .bin files

set -euo pipefail

# Source ESP-IDF environment
if [ -z "${IDF_PATH:-}" ]; then
    source "$HOME/esp/esp-idf/export.sh" 2>/dev/null
fi

VERSION_TAG="${1:-$(date +%Y%m%d)}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RELEASE_DIR="$PROJECT_DIR/release"
CMAKE_FILE="$PROJECT_DIR/CMakeLists.txt"

VARIANTS=("VERSION_1" "VERSION_2" "VERSION_2_DEBUG")
BIN_NAMES=("KaSe_V1" "KaSe_V2" "KaSe_V2_Debug")

# Save original CMakeLists.txt
cp "$CMAKE_FILE" "$CMAKE_FILE.bak"
trap 'mv "$CMAKE_FILE.bak" "$CMAKE_FILE"' EXIT

mkdir -p "$RELEASE_DIR"

for i in "${!VARIANTS[@]}"; do
    variant="${VARIANTS[$i]}"
    bin_name="${BIN_NAMES[$i]}"

    echo ""
    echo "========================================"
    echo "  Building $variant"
    echo "========================================"

    # Rewrite CMakeLists.txt with the active variant
    sed -i -E "s/^(# )?add_compile_definitions\((VERSION_1|VERSION_2_DEBUG|VERSION_2)\)/# add_compile_definitions(\2)/" "$CMAKE_FILE"
    sed -i "s/^# add_compile_definitions($variant)/add_compile_definitions($variant)/" "$CMAKE_FILE"

    idf.py fullclean > /dev/null 2>&1 || true
    idf.py build 2>&1 | tail -5

    cp "$PROJECT_DIR/build/Mae_Keyboard_Code.bin" "$RELEASE_DIR/${bin_name}_${VERSION_TAG}.bin"
    echo "  -> release/${bin_name}_${VERSION_TAG}.bin"
done

echo ""
echo "========================================"
echo "  Release binaries in $RELEASE_DIR:"
ls -lh "$RELEASE_DIR"/*_"$VERSION_TAG".bin
echo "========================================"
