/* Pinout contract — Niphargus LEFT half (U6).
 *
 * Copied by hand from docs/NIPHARGUS_V2_HARDWARE.md (netlist verified on
 * 2026-08-06). The two halves have DIFFERENT tables: these are routing
 * permutations, not a symmetry. No compilation detects an inversion, and the
 * boards have not arrived — this test is the only barrier before the bench.
 * If the contract changes, it is THIS file that gets updated first, then
 * board.h.
 */
#include "test_framework.h"

/* ESP-IDF defines GPIO_NUM_* as an enum; on host they are stubbed. */
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

#include "../boards/niphar_left/board.h"
#include "../main/comm/rf/rf_slot.h"

/* Compile-time guard: the left has had THE SAME screen as the right since
 * 2026-09-14 (Sharp LS011B7DH03 soldered on J12, portrait 68x160, CS active
 * high on GPIO14, SPI shared with the nRF24). CMakeLists.txt reads the TEXT
 * of board.h to choose the backend: a stray BOARD_DISPLAY_BACKEND_ROUND/OLED
 * here would change the build without any test flinching — compilation is
 * made to fail instead. (Until 2026-09-14 this test forbade ANY screen
 * macro: J12 was declared empty; the hardware changed, the contract follows.) */
#ifndef BOARD_DISPLAY_BACKEND_MEMLCD
#error "the left has a Sharp memory-LCD screen (J12 soldered): BOARD_DISPLAY_BACKEND_MEMLCD is missing in boards/niphar_left/board.h"
#endif
#ifdef BOARD_DISPLAY_BACKEND_ROUND
#error "the left has no round screen: BOARD_DISPLAY_BACKEND_ROUND would change the backend chosen by CMakeLists.txt"
#endif
#ifdef BOARD_DISPLAY_BACKEND_OLED
#error "the left has no OLED: BOARD_DISPLAY_BACKEND_OLED would change the backend chosen by CMakeLists.txt"
#endif

/* Unwired GPIOs: strapping and octal PSRAM. No board pin must
 * fall on these. */
static int is_forbidden(int gpio)
{
    return gpio == 3 || gpio == 45 || gpio == 46 ||
           gpio == 35 || gpio == 36 || gpio == 37;
}

/* GPIOs already committed to something other than the board's
 * matrix/peripherals: USB D-/D+ (19/20, NIPHARGUS_V2_HARDWARE.md:41) and the
 * programming connector (0, 43, 44, :47). The left exists FOR USB — a column
 * placed on GPIO19 would pass test_left_no_forbidden_gpio (not in the
 * unwired list) without this second check. */
static int is_reserved(int gpio)
{
    return gpio == 19 || gpio == 20 ||
           gpio == 0 || gpio == 43 || gpio == 44;
}

static void test_left_matrix_table(void)
{
    /* LEFT table of the contract — DO NOT copy from the right table. */
    TEST_ASSERT_EQ(ROWS0, 1,  "left row0 = GPIO1");
    TEST_ASSERT_EQ(ROWS1, 2,  "left row1 = GPIO2");
    TEST_ASSERT_EQ(ROWS2, 8,  "left row2 = GPIO8");
    TEST_ASSERT_EQ(ROWS3, 6,  "left row3 = GPIO6");

    TEST_ASSERT_EQ(COLS0, 4,  "left col0 = GPIO4");
    TEST_ASSERT_EQ(COLS1, 5,  "left col1 = GPIO5");
    TEST_ASSERT_EQ(COLS2, 7,  "left col2 = GPIO7");
    TEST_ASSERT_EQ(COLS3, 9,  "left col3 = GPIO9");
    TEST_ASSERT_EQ(COLS4, 10, "left col4 = GPIO10");
    TEST_ASSERT_EQ(COLS5, 11, "left col5 = GPIO11");
    TEST_ASSERT_EQ(COLS6, 12, "left col6 = GPIO12");
}

static void test_left_matrix_geometry(void)
{
    TEST_ASSERT_EQ(MATRIX_ROWS, 4, "4 rows");
    TEST_ASSERT_EQ(MATRIX_COLS, 7, "7 columns");
}

static void test_left_peripheral_pins(void)
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

    /* Trackpad: left only. */
    TEST_ASSERT_EQ(BOARD_TRACK_SDA_GPIO, 47, "trackpad SDA");
    TEST_ASSERT_EQ(BOARD_TRACK_SCL_GPIO, 48, "trackpad SCL");
    TEST_ASSERT_EQ(BOARD_TRACK_RDY_GPIO, 42, "trackpad RDY");
    TEST_ASSERT_EQ(BOARD_HAS_TRACKPAD_LOCAL, 1, "the trackpad is on the left");
}

static void test_left_no_forbidden_gpio(void)
{
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_TRACK_SDA_GPIO, BOARD_TRACK_SCL_GPIO, BOARD_TRACK_RDY_GPIO,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!is_forbidden(pins[i]), "no pin on an unwired GPIO");
}

static void test_left_no_reserved_gpio(void)
{
    /* USB D-/D+ and programming connector: committed elsewhere, not in the
     * unwired list so invisible to test_left_no_forbidden_gpio. */
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_TRACK_SDA_GPIO, BOARD_TRACK_SCL_GPIO, BOARD_TRACK_RDY_GPIO,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!is_reserved(pins[i]), "no pin on native USB or the programming connector");
}

static void test_left_no_pin_used_twice(void)
{
    /* A failed permutation typically produces a duplicate — across ALL the
     * board's pins (matrix + SPI + nRF + link + gauge + trackpad), not
     * just the matrix: on the right, COLS6/ROWS0 are one digit away from
     * the radio numbers, a typo that would place BOARD_NRF_CE on a
     * matrix pin would not be caught if only the matrix were checked. */
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_TRACK_SDA_GPIO, BOARD_TRACK_SCL_GPIO, BOARD_TRACK_RDY_GPIO,
    };
    const unsigned n = sizeof(pins) / sizeof(pins[0]);
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            TEST_ASSERT(pins[i] != pins[j], "no duplicate GPIO anywhere on the board");
}

static void test_left_swaps_the_link_uart(void)
{
    /* Straight cable: TX arrives on TX. ONE half must swap TXD/RXD via the
     * GPIO matrix — the left was chosen. The right must NOT swap
     * (checked in test_niphar_right_pins.c). */
    TEST_ASSERT_EQ(BOARD_LINK_SWAP_TX_RX, 1, "the left swaps TX/RX");
}

/* RF stack consumers (comm/rf/kbd_relay_tx.c) build their config from
 * BOARD_NRF_SPI_SCK, BOARD_NRF_CSN_GPIO... These names differ from the
 * hardware contract's (BOARD_NRF_SCK, BOARD_NRF_CSN), and kbd_relay_tx.c
 * provides a fallback block — GPIO 35/36/37, forbidden here — under
 * `#ifndef BOARD_NRF_SPI_HOST`.
 *
 * The Niphargus board.h files DEFINE BOARD_NRF_SPI_HOST: the guard is
 * therefore false, the whole fallback block is skipped, and the aliases are
 * defined nowhere. The failure mode is not a silent bad pinout but a
 * compilation error, the day an RF stack consumer gets compiled for a half.
 * That is less serious, it still needs closing.
 *
 * boards/conchodytes/board.h had to add this alias block for the same
 * reason, documenting the trap. Both halves had forgotten it. This test
 * locks the equivalence: a missing alias fails to compile, a drifting alias
 * fails here — not on the bench, six months later.
 *
 * NEITHER channel NOR address suffix is deliberately asserted here: unlike
 * the mouse which has only one link, a half has two (PRX toward the other
 * half, PTX toward the dongle). Channel selection belongs to B3/B4. */
static void test_left_radio_pin_aliases(void)
{
    TEST_ASSERT_EQ(BOARD_NRF_SPI_SCK,  BOARD_NRF_SCK,  "SCK  alias == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_MISO, BOARD_NRF_MISO, "MISO alias == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_MOSI, BOARD_NRF_MOSI, "MOSI alias == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_CSN_GPIO, BOARD_NRF_CSN,  "CSN  alias == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_CE_GPIO,  BOARD_NRF_CE,   "CE   alias == raw pin");
    TEST_ASSERT_EQ(BOARD_NRF_IRQ_GPIO, BOARD_NRF_IRQ,  "IRQ  alias == raw pin");

    /* Master -> dongle link (B4). comm/rf/kbd_relay_tx.c consumes these two
     * macros to address the dongle; they must agree with
     * boards/kase_dongle/board_rf.h (radio 1) or the link never establishes.
     * This is NOT the right -> left link, which will have its own macros at B3. */
    /* Hardcoded in board.h: it is tied to the channel plan, otherwise the two
     * drift silently and the link goes quiet without any test flinching. */
    TEST_ASSERT_EQ(BOARD_NRF_CHANNEL, RF_CH_KBD_DONGLE, "channel == channel plan");
    TEST_ASSERT_EQ(BOARD_NRF_CHANNEL,     0x4C, "keyboard slot channel, see the dongle's board_rf.h");
    TEST_ASSERT_EQ(BOARD_NRF_ADDR_SUFFIX, 0x01, "address suffix = keyboard slot (rf_slot.h)");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_CLOCK_HZ, BOARD_NRF_CLOCK_HZ, "SPI clock alias");
}

/* The left's screen is the SAME module as the right's, on the same CS:
 * test_niphar_right_pins.c checks 14 / active high on its side; the two
 * halves must stay aligned (same driver, same shared bus). */
static void test_ecran_memlcd_gauche(void)
{
    TEST_ASSERT_EQ(BOARD_LCD_CS_GPIO, 14, "Sharp screen CS (same as the right)");
    TEST_ASSERT_EQ(BOARD_LCD_CS_ACTIVE_HIGH, 1, "CS active HIGH, not low");
    TEST_ASSERT_EQ(BOARD_DISPLAY_WIDTH, 68, "portrait: 68 px wide");
    TEST_ASSERT_EQ(BOARD_DISPLAY_HEIGHT, 160, "portrait: 160 px tall");
    TEST_ASSERT(BOARD_LCD_CS_GPIO != BOARD_NRF_CSN && BOARD_LCD_CS_GPIO != BOARD_NRF_CE,
                "the screen CS is neither the radio's CSN nor its CE (shared bus)");
}

void test_niphar_left_pins(void)
{
    printf("\n-- Niphargus LEFT pinout (netlist contract 2026-08-06) --\n");
    test_left_matrix_table();
    test_left_matrix_geometry();
    test_left_peripheral_pins();
    test_left_no_forbidden_gpio();
    test_left_no_reserved_gpio();
    test_left_no_pin_used_twice();
    test_left_swaps_the_link_uart();
    test_left_radio_pin_aliases();
    test_ecran_memlcd_gauche();
}
