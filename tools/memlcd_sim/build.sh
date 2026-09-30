#!/usr/bin/env bash
# Host renderer of the left half's memory-LCD screen (see README.md).
#
# Compiles the REAL LVGL 8 sources the firmware uses (managed_components/
# lvgl__lvgl, fetched by `idf.py reconfigure` or any build) with this
# folder's lv_conf.h, then builds and runs, in order:
#   1. check_glyph_ink  — GATE: every font the screen uses keeps its ink
#                         through the panel's 1-bit threshold (12 px floor)
#   2. test_safe_wrap   — GATE: no name line is ever digits without a letter
#   3. render_cave_dark — the 10 reference states + proof sheets -> out/
#   4. render_gauge_lab — the water-drop battery gauge sheet -> out/gauges/
# Any gate failing stops the script with a non-zero status.
#
# Usage: tools/memlcd_sim/build.sh [--relib]   (--relib rebuilds liblvgl.a)
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
if [ ! -f "$BUILD/liblvgl.a" ] || [ "${1:-}" = "--relib" ] || [ "$SIM/lv_conf.h" -nt "$BUILD/liblvgl.a" ]; then
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

CFLAGS=(-O1 -w -I"$SIM" -I"$SIM/assets" -I"$LVGL"
        -I"$REPO/main/display/memlcd" -I"$REPO/main/comm/chest" -I"$REPO/main/security")
PURE=("$REPO/main/comm/chest/chest_proto.c" "$REPO/main/security/cr_crc16.c")
LIB=("$BUILD/liblvgl.a" -lm)

echo "== 1. glyph ink (gate) =="
"$CC" "${CFLAGS[@]}" "$SIM/check_glyph_ink.c" "${LIB[@]}" -o "$BUILD/check_glyph_ink"
"$BUILD/check_glyph_ink"

echo "== 2. safe wrap (gate) =="
"$CC" "${CFLAGS[@]}" "$SIM/test_safe_wrap.c" "${LIB[@]}" -o "$BUILD/test_safe_wrap"
"$BUILD/test_safe_wrap"

cd "$SIM"   # the renderers write under out/, relative
echo "== 3. cave dark: the 10 reference states =="
"$CC" "${CFLAGS[@]}" main_cave_dark.c cave_ui.c dir_cave_dark.c common.c states.c "${PURE[@]}" \
    assets/img_niphargus_28.c assets/img_niphargus_56.c assets/img_rock.c "${LIB[@]}" -o "$BUILD/render_cave_dark"
"$BUILD/render_cave_dark"

echo "== 4. water-drop gauge =="
"$CC" "${CFLAGS[@]}" main_gauge_lab.c gauge_lab.c common.c assets/img_niphargus_28.c assets/img_rock.c \
    "${LIB[@]}" -o "$BUILD/render_gauge_lab"
"$BUILD/render_gauge_lab"

echo "memlcd_sim: done — images under $OUT"
