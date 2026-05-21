/* kase_half_right/board.h
 *
 * Inherit full left board definition — all matrix GPIO, NRF SPI bus,
 * timing constants, USB IDs, etc. are identical (reversible PCB).
 * Only the side identifier and RF channel/address differ.
 *
 * No include guard here: the parent board.h (kase_half_left) provides
 * the guard. This follows the kase_v2_debug precedent. */
#include "../kase_half_left/board.h"

/* ── Override product name ─────────────────────────────────── */
#undef  PRODUCT_NAME
#define PRODUCT_NAME        "KaSe Half Right"
#undef  MODULE_ID
#define MODULE_ID           0x02   /* right half */

/* ── Override side identifier ──────────────────────────────── */
#undef  HALF_SIDE_LEFT
#undef  HALF_SIDE
#define HALF_SIDE_RIGHT     1
#define HALF_SIDE           HALF_SIDE_RIGHT

/* ── Override RF addressing — right half ───────────────────── */
/* Must match dongle board_rf.h: RF_CH_RIGHT_DEFAULT=0x52, suffix=0x02 */
#undef  BOARD_NRF_ADDR_SUFFIX
#undef  BOARD_NRF_CHANNEL
#define BOARD_NRF_ADDR_SUFFIX   0x02
#define BOARD_NRF_CHANNEL       0x52   /* 2482 MHz */
