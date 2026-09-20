/* Pinout contract — Niphargus RIGHT half (U5).
 *
 * Copied by hand from docs/NIPHARGUS_V2_HARDWARE.md (netlist verified on
 * 2026-08-06). The two halves have DIFFERENT tables: these are
 * routing permutations, not a symmetry. No compilation detects an
 * inversion, and the boards haven't arrived yet — this test is the only barrier
 * before the bench. If the contract changes, THIS file is the one updated
 * first, then board.h.
 */
#include "test_framework.h"

/* ESP-IDF defines GPIO_NUM_* as an enum; on host we stub them. */
#ifndef GPIO_NUM_0
#define GPIO_NUM_0  0
#define GPIO_NUM_1  1
#define GPIO_NUM_2  2
#define GPIO_NUM_4  4
#define GPIO_NUM_5  5
#define GPIO_NUM_6  6
#define GPIO_NUM_7  7
#define GPIO_NUM_8  8
#define GPIO_NUM_9  9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_21 21
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
#define GPIO_NUM_NC (-1)
#define SPI2_HOST   1
#endif

#include "../boards/niphar_right/board.h"
#include "../main/comm/rf/rf_packet.h"

/* Compile-time guard: the trackpad is LEFT only (Azoteq TPS43,
 * I2C + RDY, see docs/NIPHARGUS_V2_HARDWARE.md). A stray
 * BOARD_HAS_TRACKPAD_LOCAL macro here would describe a peripheral that doesn't
 * exist on this half — we make compilation fail rather than
 * let it pass silently. */
#ifdef BOARD_HAS_TRACKPAD_LOCAL
#error "le trackpad est sur la gauche uniquement : BOARD_HAS_TRACKPAD_LOCAL n'a rien à faire dans boards/niphar_right/board.h"
#endif

/* Unwired GPIO: strapping and octal PSRAM. No board pin should
 * fall on these. */
static int is_forbidden(int gpio)
{
    return gpio == 3 || gpio == 45 || gpio == 46 ||
           gpio == 35 || gpio == 36 || gpio == 37;
}

/* GPIO already committed to something other than the board's matrix/peripherals:
 * USB D-/D+ (19/20, NIPHARGUS_V2_HARDWARE.md:41) and the programming
 * connector (0, 43, 44, :47). Since COLS6 = GPIO1 and ROWS0 = GPIO2 are already
 * one digit away from the radio numbers, a second check here covers committed
 * pins that test_right_no_forbidden_gpio doesn't see (not in the unwired
 * list). */
static int is_reserved(int gpio)
{
    return gpio == 19 || gpio == 20 ||
           gpio == 0 || gpio == 43 || gpio == 44;
}

static void test_right_matrix_table(void)
{
    /* RIGHT table of the contract — different from the left, this is NOT a
     * symmetry: of the matrix's 11 pins, only one coincides between
     * the two halves (col3 = GPIO9); the other 10 differ. */
    TEST_ASSERT_EQ(ROWS0, 2,  "right row0 = GPIO2");
    TEST_ASSERT_EQ(ROWS1, 12, "right row1 = GPIO12");
    TEST_ASSERT_EQ(ROWS2, 4,  "right row2 = GPIO4");
    TEST_ASSERT_EQ(ROWS3, 5,  "right row3 = GPIO5");

    TEST_ASSERT_EQ(COLS0, 6,  "right col0 = GPIO6");
    TEST_ASSERT_EQ(COLS1, 7,  "right col1 = GPIO7");
    TEST_ASSERT_EQ(COLS2, 8,  "right col2 = GPIO8");
    TEST_ASSERT_EQ(COLS3, 9,  "right col3 = GPIO9");
    TEST_ASSERT_EQ(COLS4, 11, "right col4 = GPIO11");
    TEST_ASSERT_EQ(COLS5, 10, "right col5 = GPIO10");
    TEST_ASSERT_EQ(COLS6, 1,  "right col6 = GPIO1");
}

static void test_right_matrix_geometry(void)
{
    TEST_ASSERT_EQ(MATRIX_ROWS, 4, "4 rows");
    TEST_ASSERT_EQ(MATRIX_COLS, 7, "7 columns");
}

static void test_right_peripheral_pins(void)
{
    TEST_ASSERT_EQ(BOARD_NRF_SCK,  38, "shared SPI SCK");
    TEST_ASSERT_EQ(BOARD_NRF_MISO, 39, "shared SPI MISO");
    TEST_ASSERT_EQ(BOARD_NRF_MOSI, 40, "shared SPI MOSI");
    TEST_ASSERT_EQ(BOARD_NRF_CE,   15, "nRF24 CE");
    TEST_ASSERT_EQ(BOARD_NRF_CSN,  16, "nRF24 CSN");
    TEST_ASSERT_EQ(BOARD_NRF_IRQ,  41, "nRF24 IRQ");

    TEST_ASSERT_EQ(BOARD_LINK_TX,    17, "TRRS TX (UART1)");
    TEST_ASSERT_EQ(BOARD_LINK_RX,    18, "TRRS RX (UART1)");
    TEST_ASSERT_EQ(BOARD_LINK_5V_EN, 21, "SiP32431 ON");

    TEST_ASSERT_EQ(BOARD_VBAT_SENSE_GPIO, 13, "ADC2_CH2 gauge");
}

static void test_right_display_pins(void)
{
    /* Sharp LS011B7DH03: CS ACTIVE HIGH, write-only, LSB-first. */
    TEST_ASSERT_EQ(BOARD_LCD_CS_GPIO, 14, "Sharp screen's CS");
    TEST_ASSERT_EQ(BOARD_LCD_CS_ACTIVE_HIGH, 1, "CS active HIGH, not low");
    /* The screen shares the nRF24's SPI. */
    TEST_ASSERT_EQ(BOARD_NRF_SCK,  38, "shared SPI SCK screen + radio");
    TEST_ASSERT_EQ(BOARD_NRF_MOSI, 40, "shared SPI MOSI screen + radio");
}

static void test_right_no_forbidden_gpio(void)
{
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_LCD_CS_GPIO,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!is_forbidden(pins[i]), "no pin on an unwired GPIO");
}

static void test_right_no_reserved_gpio(void)
{
    /* USB D-/D+ and programming connector: committed elsewhere, not in the
     * unwired list so invisible to test_right_no_forbidden_gpio. */
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_LCD_CS_GPIO,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!is_reserved(pins[i]), "no pin on the native USB or the programming connector");
}

static void test_right_no_pin_used_twice(void)
{
    /* A botched permutation typically produces a duplicate — across ALL the
     * board's pins (matrix + SPI + nRF + link + gauge + screen), not
     * just the matrix: COLS6 = GPIO1 and ROWS0 = GPIO2 are one digit
     * away from the radio numbers (CE=15/CSN=16), a typo that would
     * put BOARD_NRF_CE on GPIO12 wouldn't be seen if we only looked
     * at the matrix. */
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_LCD_CS_GPIO,
    };
    const unsigned n = sizeof(pins) / sizeof(pins[0]);
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            TEST_ASSERT(pins[i] != pins[j], "no duplicate GPIO across the whole board");
}

static void test_right_does_not_swap_the_link_uart(void)
{
    /* It's the LEFT that swaps. If both swap, or neither, two TX end up
     * conflicting on the same wire. */
    TEST_ASSERT_EQ(BOARD_LINK_SWAP_TX_RX, 0, "the right does not swap");
}

/* RF stack consumers (comm/rf/kbd_relay_tx.c) build their
 * config from BOARD_NRF_SPI_SCK, BOARD_NRF_CSN_GPIO... These names differ from
 * those of the hardware contract (BOARD_NRF_SCK, BOARD_NRF_CSN), and kbd_relay_tx.c
 * provides a fallback block — GPIO 35/36/37, forbidden here — under
 * `#ifndef BOARD_NRF_SPI_HOST`.
 *
 * The Niphargus board.h files DEFINE BOARD_NRF_SPI_HOST: the guard is therefore
 * false, the entire fallback block is skipped, and the aliases are defined
 * nowhere. The failure mode isn't a silent bad pinout but
 * a compile error, the day an RF stack consumer gets
 * compiled for a half. That's less serious, it still needs closing.
 *
 * boards/conchodytes/board.h had to add this alias block for the same reason,
 * documenting the trap. Both halves had forgotten it. This test
 * locks in the equivalence: a missing alias doesn't compile, a drifting alias
 * fails here — not on the bench, six months later.
 *
 * We deliberately assert NEITHER channel NOR address suffix: unlike
 * the mouse which has only one link, a half has two (PRX toward the other half,
 * PTX toward the dongle). Channel choice belongs to B3/B4. */
static void test_right_radio_pin_aliases(void)
{
    TEST_ASSERT_EQ(BOARD_NRF_SPI_SCK,  BOARD_NRF_SCK,  "alias SCK  == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_MISO, BOARD_NRF_MISO, "alias MISO == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_MOSI, BOARD_NRF_MOSI, "alias MOSI == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_CSN_GPIO, BOARD_NRF_CSN,  "alias CSN  == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_CE_GPIO,  BOARD_NRF_CE,   "alias CE   == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_IRQ_GPIO, BOARD_NRF_IRQ,  "alias IRQ  == raw pin");
}

/* The RF protocol's half-matrix geometry must match the
 * board's. rf_packet.h says so itself — "must match board.h half dimensions" —
 * but nothing verified it, and the value there remained that of the old
 * KaSe halves (5x7), removed from the repo at commit c107df77. It therefore
 * described hardware that no longer exists.
 *
 * Concrete consequences: one wasted bitmap byte per packet, and a
 * `row < RF_HALF_ROWS` validation that would accept a nonexistent row 4 on
 * a 4x7 matrix. No live impact as long as heartbeat.c isn't compiled,
 * but B3 will rely on this contract — better to make it true beforehand.
 *
 * This test ties the two together: any future divergence breaks here. */
static void test_right_rf_geometry_matches_board(void)
{
    TEST_ASSERT_EQ(RF_HALF_ROWS, MATRIX_ROWS, "RF geometry: rows == board.h");
    TEST_ASSERT_EQ(RF_HALF_COLS, MATRIX_COLS, "RF geometry: columns == board.h");
    /* The bitmap must cover exactly the matrix, with no dead byte. */
    TEST_ASSERT_EQ(RF_HALF_BITMAP_BYTES, (MATRIX_ROWS * MATRIX_COLS + 7) / 8,
                   "bitmap = ceil(rows*cols/8)");
}

void test_niphar_right_pins(void)
{
    printf("\n-- brochage Niphargus DROITE (contrat netlist 2026-08-06) --\n");
    test_right_matrix_table();
    test_right_matrix_geometry();
    test_right_peripheral_pins();
    test_right_display_pins();
    test_right_no_forbidden_gpio();
    test_right_no_reserved_gpio();
    test_right_no_pin_used_twice();
    test_right_does_not_swap_the_link_uart();
    test_right_radio_pin_aliases();
    test_right_rf_geometry_matches_board();
}
