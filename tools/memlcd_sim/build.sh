#!/usr/bin/env bash
# Host renderer of the left half's memory-LCD screen (see README.md).
#
# Compiles the REAL LVGL 8 sources the firmware uses (managed_components/
# lvgl__lvgl, fetched by `idf.py reconfigure` or any build) with this
# folder's lv_conf.h, and the FIRMWARE's own left-screen engine
# (main/display/memlcd/memlcd_cave.c + its assets) — no copy of it lives
# here. Then builds and runs, in order:
#   1. check_widths     — GATE: the layout's width oracle (memlcd_model.h,
#                         kerning included) == lv_txt_get_width, every font
#   2. check_glyph_ink  — GATE: every font the engine uses keeps its ink
#                         through the panel's 1-bit threshold (12 px floor)
#   3. test_safe_wrap   — GATE: the firmware's safe wrap, re-measured by LVGL
#   4. render_cave      — the 10 reference states and the proof sheets -> out/
# Any gate failing stops the script with a non-zero status.
#
# Usage: tools/memlcd_sim/build.sh [--relib] [--gates]
#   --relib  rebuilds liblvgl.a
#   --gates  runs the three gates only, no rendering (what the tripwire brick
#            scripts/tripwire.d/memlcd-sim-gates.sh calls from check.sh)
set -euo pipefail
SIM="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SIM/../.." && pwd)"
LVGL="$REPO/managed_components/lvgl__lvgl"
BUILD="$SIM/build"
OUT="$SIM/out"
CC="${CC:-gcc}"

if [ ! -d "$LVGL/src" ]; then
    echo "memlcd_sim: $LVGL is missing." >&2
    echo "  Fetch the managed components first, e.g. inside the ESP-IDF devshell:" >&2
    echo "  idf.py -B build_niphar_left -DBOARD=niphar_left -DSDKCONFIG=build_niphar_left/sdkconfig reconfigure" >&2
    exit 2
fi

mkdir -p "$BUILD/obj" "$OUT/cave_dark" "$OUT/gauges"

# ---- LVGL, incremental: an object is rebuilt when its source or lv_conf.h is newer ----
RELIB=0; GATES_ONLY=0
for a in "$@"; do
    case "$a" in
        --relib) RELIB=1 ;;
        --gates) GATES_ONLY=1 ;;
        *) echo "memlcd_sim: unknown argument $a" >&2; exit 2 ;;
    esac
done
if [ ! -f "$BUILD/liblvgl.a" ] || [ "$RELIB" = 1 ] || [ "$SIM/lv_conf.h" -nt "$BUILD/liblvgl.a" ]; then
    echo "== liblvgl.a from $LVGL (read-only) =="
    objs=()
    while IFS= read -r f; do
        o="$BUILD/obj/$(echo "${f#"$LVGL"/}" | tr '/' '_').o"
        if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$SIM/lv_conf.h" -nt "$o" ]; then
            "$CC" -O1 -w -I"$SIM" -I"$LVGL" -c "$f" -o "$o"
        fi
        objs+=("$o")
    done < <(find "$LVGL/src" -name '*.c' | sort)
    rm -f "$BUILD/liblvgl.a"
    ar rcs "$BUILD/liblvgl.a" "${objs[@]}"
fi

MEMLCD="$REPO/main/display/memlcd"
CFLAGS=(-O1 -w -I"$SIM" -I"$LVGL" -I"$MEMLCD" -I"$REPO/main/comm/chest" -I"$REPO/main/security")
PURE=("$REPO/main/comm/chest/chest_proto.c" "$REPO/main/security/cr_crc16.c")
ENGINE=("$MEMLCD/memlcd_cave.c" "$MEMLCD/memlcd_assets_cave.c"
        "$REPO/main/display/assets/img_niphargus_28.c" "$REPO/main/display/assets/img_niphargus_56.c")
LIB=("$BUILD/liblvgl.a" -lm)

echo "== 1. width oracle == LVGL (gate) =="
"$CC" "${CFLAGS[@]}" "$SIM/check_widths.c" "${LIB[@]}" -o "$BUILD/check_widths"
"$BUILD/check_widths"

echo "== 2. glyph ink of the engine's fonts (gate) =="
"$CC" "${CFLAGS[@]}" "$SIM/check_glyph_ink.c" "${ENGINE[@]}" "${PURE[@]}" "${LIB[@]}" -o "$BUILD/check_glyph_ink"
"$BUILD/check_glyph_ink"

echo "== 3. safe wrap, measured by LVGL (gate) =="
"$CC" "${CFLAGS[@]}" "$SIM/test_safe_wrap.c" "${LIB[@]}" -o "$BUILD/test_safe_wrap"
"$BUILD/test_safe_wrap"

if [ "$GATES_ONLY" = 1 ]; then
    echo "memlcd_sim: the three gates passed"
    exit 0
fi

cd "$SIM"   # the renderer writes under out/, relative
echo "== 4. the firmware engine: reference states and proofs =="
"$CC" "${CFLAGS[@]}" main_cave.c common.c states.c "${ENGINE[@]}" "${PURE[@]}" "${LIB[@]}" -o "$BUILD/render_cave"
"$BUILD/render_cave"

echo "memlcd_sim: done — images under $OUT"
