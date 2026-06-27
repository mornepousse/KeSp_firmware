#include "../kase_v2/board.h"

/* Override GPIOs that differ on the debug board */
#undef COLS7
#define COLS7  GPIO_NUM_21

#undef COLS8
#define COLS8  GPIO_NUM_4    /* unchanged (V2D matrix col 8) */

/* ── NRF24L01+ — V2D wireless relay to the M.2 dongle ──────────
 * Physical wiring on this bodged V2D (cut traces + flying wires, 2026-06-27).
 * Read by comm/rf/kbd_relay_tx.c — defining BOARD_NRF_SPI_HOST here skips its
 * built-in fallback pins. MISO is on GPIO38 (COLS8 stays on GPIO4).
 * GPIO45 (IRQ) is a strapping pin — fine as a post-boot input. */
#define BOARD_NRF_SPI_HOST      SPI2_HOST
#define BOARD_NRF_SPI_MOSI      GPIO_NUM_5
#define BOARD_NRF_SPI_MISO      GPIO_NUM_37
#define BOARD_NRF_SPI_SCK       GPIO_NUM_6
#define BOARD_NRF_SPI_CLOCK_HZ  (8 * 1000 * 1000)
#define BOARD_NRF_CSN_GPIO      GPIO_NUM_48
#define BOARD_NRF_CE_GPIO       GPIO_NUM_47
#define BOARD_NRF_IRQ_GPIO      GPIO_NUM_45
#define BOARD_NRF_CHANNEL       76
#define BOARD_NRF_ADDR_SUFFIX   0x03

/* Override product info */
#undef GATTS_TAG
#define GATTS_TAG   "KaSe_V2_DBG"

#undef PRODUCT_NAME
#define PRODUCT_NAME  "KaSe V2 Debug"
