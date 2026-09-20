/* Physical layout of the dongle in FUSION mode — redirected to the LEFT half.
 *
 * Same logic as board_keymap.c: a single 2D geometry, the left half's
 * (boards/niphar_layout.inc). The internal `#include "../niphar_layout.inc"`
 * resolves relative to niphar_left/board_layout.c, so it correctly points to
 * boards/niphar_layout.inc regardless of the includer. Compiled only under
 * KASE_DONGLE_FUSION (CMake). */
#include "../niphar_left/board_layout.c"
