/* Default dongle keymap in FUSION mode — redirected to the LEFT half.
 *
 * In KASE_DONGLE_FUSION mode the dongle runs the keymap engine and shares
 * the left half's default keymap: a single source of truth, never
 * two that could diverge (the design's "same code" rule). This file is only
 * compiled when the engine is (CMake: NOT CONFIG_KASE_NO_KEYMAP_ENGINE, true on
 * the dongle only under fusion). Outside fusion, it does not enter the build.
 *
 * board.h resolves to the dongle's (fusion dimensions), not the left half's
 * — that's intentional: same values, dongle pinout.
 *
 * Design: docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md */
#include "../niphar_left/board_keymap.c"
