#!/usr/bin/env bash
# Builds the release binaries for the 3 keyboards and gathers them in release/.
#
# Usage: ./scripts/build_release.sh vX.Y.Z
# Output: release/KaSe_<version>_<HW>.bin       (app only, flash at 0x20000)
#         release/KaSe_<version>_<HW>_full.bin  (full image, flash at 0x0)
#
# ── Why this script was rewritten (2026-08-19) ───────────────────────────────
# The previous version ran `idf.py -DBOARD=<board> build` without -B or
# -DSDKCONFIG: all three boards were built in the SAME build/ folder with the
# root sdkconfig. Two consequences, both silent:
#
#   1. Config leakage from one board to another — which CLAUDE.md explicitly
#      forbids.
#   2. Per-board `sdkconfig.defaults.<short>` were NEVER read, because they
#      are only read when generating a fresh sdkconfig, and a root sdkconfig
#      already existed. The V2D would come out with BLE compiled in despite
#      sdkconfig.defaults.v2_debug.
#
# Hence: one build folder AND one sdkconfig per board, like the rest of the repo.
set -euo pipefail

if [ -z "${IDF_PATH:-}" ]; then
    source "$HOME/esp/esp-idf/export.sh" 2>/dev/null
fi
export IDF_CCACHE_ENABLE=1

VERSION_TAG="${1:-$(git describe --tags --always)}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RELEASE_DIR="$PROJECT_DIR/release"

BOARDS=("kase_v1" "kase_v2" "kase_v2_debug" "kase_dongle" "niphar_left" "niphar_right" "conchodytes")
HW_NAMES=("V1" "V2" "V2_Debug" "Dongle" "Niphargus_Left" "Niphargus_Right" "Conchodytes")
# Nix devshell ships `esptool`, the classic install ships `esptool.py`.
ESPTOOL="$(command -v esptool || command -v esptool.py)"
export IDF_COMPONENT_CHECK_NEW_VERSION=0

mkdir -p "$RELEASE_DIR"
cd "$PROJECT_DIR"

for i in "${!BOARDS[@]}"; do
    board="${BOARDS[$i]}"
    hw="${HW_NAMES[$i]}"
    bdir="build_${board}"

    echo ""
    echo "======================================== $board"
    # reconfigure: the version string (git describe) is captured at CMake
    # configure time — an incremental build after `git tag` keeps the old one.
    idf.py -B "$bdir" -DBOARD="$board" -DSDKCONFIG="$bdir/sdkconfig" reconfigure > /dev/null 2>&1
    idf.py -B "$bdir" -DBOARD="$board" -DSDKCONFIG="$bdir/sdkconfig" build 2>&1 | tail -3

    cp "$bdir/KeSp.bin" "$RELEASE_DIR/KaSe_${VERSION_TAG}_${hw}.bin"

    # Full image: bootloader + partition table + otadata + app + storage.
    # Flashes at 0x0 after an erase_flash; required after any partition
    # table change.
    # The dongle and the mouse have no LittleFS image (no keymap storage): the
    # storage partition is left blank in their full image.
    storage_arg=()
    [ -f "$bdir/storage.bin" ] && storage_arg=(0x420000 storage.bin)
    ( cd "$bdir" && "$ESPTOOL" --chip esp32s3 merge_bin \
        -o "$RELEASE_DIR/KaSe_${VERSION_TAG}_${hw}_full.bin" \
        --flash_mode dio --flash_freq 80m --flash_size 16MB \
        0x0 bootloader/bootloader.bin \
        0x8000 partition_table/partition-table.bin \
        0x19000 ota_data_initial.bin \
        0x20000 KeSp.bin \
        "${storage_arg[@]}" > /dev/null )

    echo "  -> release/KaSe_${VERSION_TAG}_${hw}.bin (+ _full)"
done

echo ""
echo "======================================== artefacts"
ls -lh "$RELEASE_DIR"/KaSe_"$VERSION_TAG"_*.bin | awk '{print "  "$5"\t"$9}'

# Safeguard: the V2D must not embed BLE (sdkconfig.defaults.v2_debug).
# If this check fails, the per-board defaults were not picked up — exactly
# the silent failure this script's rewrite fixes.
if grep -q "^CONFIG_BT_ENABLED=y" build_kase_v2_debug/sdkconfig 2>/dev/null; then
    echo ""
    echo "ERROR: the V2D embeds BLE — sdkconfig.defaults.v2_debug was not picked up." >&2
    echo "       Delete build_kase_v2_debug/sdkconfig and rerun." >&2
    exit 1
fi
echo "  ✓ V2D without BLE (per-board defaults correctly applied)"
