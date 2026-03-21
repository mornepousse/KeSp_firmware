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

BOARDS=("kase_v1" "kase_v2" "kase_v2_debug")
HW_NAMES=("V1" "V2" "V2_Debug")

mkdir -p "$RELEASE_DIR"

for i in "${!BOARDS[@]}"; do
    board="${BOARDS[$i]}"
    hw_name="${HW_NAMES[$i]}"

    echo ""
    echo "========================================"
    echo "  Building $board"
    echo "========================================"

    idf.py -DBOARD="$board" fullclean > /dev/null 2>&1 || true
    idf.py -DBOARD="$board" build 2>&1 | tail -5

    cp "$PROJECT_DIR/build/KeSp.bin" "$RELEASE_DIR/KaSe_${VERSION_TAG}_${hw_name}.bin"
    echo "  -> release/KaSe_${VERSION_TAG}_${hw_name}.bin"
done

echo ""
echo "========================================"
echo "  Release binaries in $RELEASE_DIR:"
ls -lh "$RELEASE_DIR"/KaSe_"$VERSION_TAG"_*.bin
echo "========================================"
