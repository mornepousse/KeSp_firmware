/* Pinout contract — Conchodytes mouse (U6).
 *
 * Read from the netlist at ~/Documents/GitHub/Conchodytes/hardware/pcb/ via
 * `kicad-cli export netlist` on 2026-08-25, NOT copied from the README: the
 * latter swapped left and right on the NC contacts (fixed since, but the
 * netlist remains the source of truth).
 *
 * This file exists for a specific reason: on this board, a pinout swap shows
 * up neither at compile time, nor at boot, nor even under naive use. The six
 * click contacts go in NO+NC pairs of the SAME button, and it is this
 * pairing that removes the bounce; crossing two pairs gives a firmware that
 * appears to work and produces phantom clicks. If the contract changes,
 * THIS file is updated first, then board.h.
 *
 * The values below were verified on real hardware on 2026-08-25:
 * pressing only the left click -> 26 edges on L, 0 on R.
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

#include "../boards/conchodytes/board.h"
#include "../main/comm/rf/rf_slot.h"

/* Compile-time guards: the mouse has no matrix, no display, no trackpad.
 * A stray macro here would describe a peripheral that does not exist on
 * this board — we fail the build rather than let it slide. */
#ifdef BOARD_HAS_TRACKPAD_LOCAL
#error "la souris n'a pas de trackpad : BOARD_HAS_TRACKPAD_LOCAL n'a rien a faire dans boards/conchodytes/board.h"
#endif
/* MATRIX_ROWS/COLS come from test_framework.h and are therefore always
 * defined here: we guard on ROWS0/COLS0, which only exist in a keyboard's
 * board.h. */
#ifdef ROWS0
#error "la souris n'a aucun interrupteur en matrice : ROWS0 n'a rien a faire dans boards/conchodytes/board.h"
#endif
#ifdef COLS0
#error "la souris n'a aucun interrupteur en matrice : COLS0 n'a rien a faire dans boards/conchodytes/board.h"
#endif
#ifdef BOARD_DISPLAY_BACKEND_ROUND
#error "la souris n'a pas d'ecran : BOARD_DISPLAY_BACKEND_ROUND n'a rien a faire dans boards/conchodytes/board.h"
#endif

/* GPIOs not wired on this board: strapping and octal PSRAM. */
static int conch_is_forbidden(int gpio)
{
    return gpio == 3 || gpio == 45 || gpio == 46 ||
           gpio == 35 || gpio == 36 || gpio == 37;
}

/* GPIOs committed elsewhere: native USB (19/20, module pins 13/14) and the
 * J1 programming connector (0, 43, 44). */
static int conch_is_reserved(int gpio)
{
    return gpio == 19 || gpio == 20 ||
           gpio == 0 || gpio == 43 || gpio == 44;
}

static void test_conch_sensor_pins(void)
{
    /* PMW3389: SPI shared with the nRF24, plus two dedicated lines. */
    TEST_ASSERT_EQ(BOARD_SNS_NCS_GPIO,    17, "sensor NCS = GPIO17 (pin 10)");
    TEST_ASSERT_EQ(BOARD_SNS_MOTION_GPIO, 18, "sensor MOTION = GPIO18 (pin 11)");

    /* fSCLK max = 2.0 MHz, Table 4 p. 15 of the PMW3389DM-T3QU datasheet.
     * Validated on hardware at this frequency on 2026-08-25. */
    TEST_ASSERT(BOARD_SNS_CLOCK_HZ <= 2000000, "PMW3389 fSCLK caps at 2 MHz");
    TEST_ASSERT_EQ(BOARD_SNS_SPI_MODE, 3, "PMW3389 in mode 3 (CPOL=1, CPHA=1)");
}

static void test_conch_radio_pins(void)
{
    /* The nRF24 shares SCK/MOSI/MISO with the sensor, but not the SPI mode:
     * mode 0 here, mode 3 for the PMW3389. */
    TEST_ASSERT_EQ(BOARD_NRF_SCK,  38, "SPI SCK shared sensor + radio");
    TEST_ASSERT_EQ(BOARD_NRF_MISO, 39, "SPI MISO shared");
    TEST_ASSERT_EQ(BOARD_NRF_MOSI, 40, "SPI MOSI shared");
    TEST_ASSERT_EQ(BOARD_NRF_CSN,  2,  "nRF24 CSN = GPIO2 (pin 38)");
    TEST_ASSERT_EQ(BOARD_NRF_CE,   1,  "nRF24 CE = GPIO1 (pin 39)");
    TEST_ASSERT_EQ(BOARD_NRF_IRQ,  41, "nRF24 IRQ = GPIO41 (pin 34)");

    /* Dongle slot 2. boards/kase_dongle/board.h annotates BOARD_NRF2_* as
     * "mouse slot (Conchodytes), channel 0x52" — both sides must
     * agree or the link never establishes. */
    TEST_ASSERT_EQ(BOARD_NRF_ADDR_SUFFIX, 0x02, "address suffix = mouse slot");
    TEST_ASSERT_EQ(BOARD_NRF_CHANNEL, RF_CH_MOUSE_DONGLE, "channel == channel plan");
    TEST_ASSERT_EQ(BOARD_NRF_CHANNEL,     0x52, "slot 2 channel, cf. dongle board.h");
}

static void test_conch_click_pairs(void)
{
    /* THE test of this file. Each button has its NO and its NC, and the
     * debounce reads BOTH. Crossing the pairs gives a firmware that produces
     * phantom clicks without any tool noticing.
     *
     * ⚠ The README stated LEFT_NC = GPIO4 and RIGHT_NC = GPIO5. The netlist
     * says the opposite: GPIO4 -> SW2.3 (RIGHT click, R109), GPIO5 -> SW1.3
     * (LEFT click, R108). Verified on the board: pressing only the left click
     * produced 26 edges on L and 0 on R.
     */
    TEST_ASSERT_EQ(BOARD_SW_LEFT_GPIO,     10, "left click NO = GPIO10");
    TEST_ASSERT_EQ(BOARD_SW_LEFT_NC_GPIO,   5, "left click NC = GPIO5, NOT GPIO4");
    TEST_ASSERT_EQ(BOARD_SW_RIGHT_GPIO,    11, "right click NO = GPIO11");
    TEST_ASSERT_EQ(BOARD_SW_RIGHT_NC_GPIO,  4, "right click NC = GPIO4, NOT GPIO5");
    TEST_ASSERT_EQ(BOARD_SW_MID_GPIO,      12, "middle click NO = GPIO12");
    TEST_ASSERT_EQ(BOARD_SW_MID_NC_GPIO,    6, "middle click NC = GPIO6");

    /* The three NCs are on 4/5/6 and the three NOs on 10/11/12: no NC should
     * end up in the NO range, and vice versa. A typo swapping an NO and an NC
     * would produce a button whose two lines are on the same side — invisible
     * to the equalities above if they were all updated together. */
    const int nc[] = { BOARD_SW_LEFT_NC_GPIO, BOARD_SW_RIGHT_NC_GPIO, BOARD_SW_MID_NC_GPIO };
    const int no[] = { BOARD_SW_LEFT_GPIO,    BOARD_SW_RIGHT_GPIO,    BOARD_SW_MID_GPIO };
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT(nc[i] >= 4  && nc[i] <= 6,  "the three NCs are on GPIO4-6");
        TEST_ASSERT(no[i] >= 10 && no[i] <= 12, "the three NOs are on GPIO10-12");
    }
}

static void test_conch_wheel_pins(void)
{
    TEST_ASSERT_EQ(BOARD_ENC_A_GPIO, 7, "encoder channel A = GPIO7 (pin 7)");
    TEST_ASSERT_EQ(BOARD_ENC_B_GPIO, 9, "encoder channel B = GPIO9 (pin 17)");
}

static void test_conch_battery_pin(void)
{
    /* WARNING: GPIO13 = ADC2_CH2, and on ESP32-S3 ADC2 is shared with the
     * WiFi driver: as long as the battery reading is there, WiFi is forbidden
     * on this board. No consequence today (the radio is an nRF24 and the
     * firmware never turns WiFi on) but it is a mortgage.
     * NOTES-V2.md point 7 plans to move the reading to GPIO8 / ADC1_CH7
     * in v2. When that is done, this value changes first. */
    TEST_ASSERT_EQ(BOARD_VBAT_SENSE_GPIO, 13, "gauge on ADC2_CH2 in v1");
}

/* All the board's pins, in a single place — the next three tests
 * depend on it and must see exactly the same list. */
#define CONCH_ALL_PINS \
    BOARD_SNS_NCS_GPIO, BOARD_SNS_MOTION_GPIO, \
    BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI, \
    BOARD_NRF_CSN, BOARD_NRF_CE, BOARD_NRF_IRQ, \
    BOARD_SW_LEFT_GPIO,  BOARD_SW_LEFT_NC_GPIO, \
    BOARD_SW_RIGHT_GPIO, BOARD_SW_RIGHT_NC_GPIO, \
    BOARD_SW_MID_GPIO,   BOARD_SW_MID_NC_GPIO, \
    BOARD_ENC_A_GPIO, BOARD_ENC_B_GPIO, \
    BOARD_VBAT_SENSE_GPIO

static void test_conch_no_forbidden_gpio(void)
{
    const int pins[] = { CONCH_ALL_PINS };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!conch_is_forbidden(pins[i]), "no pin on an unwired GPIO");
}

static void test_conch_no_reserved_gpio(void)
{
    const int pins[] = { CONCH_ALL_PINS };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!conch_is_reserved(pins[i]), "no pin on native USB or J1");
}

static void test_conch_no_pin_used_twice(void)
{
    const int pins[] = { CONCH_ALL_PINS };
    const unsigned n = sizeof(pins) / sizeof(pins[0]);
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            TEST_ASSERT(pins[i] != pins[j], "no GPIO duplicated across the whole board");
}

void test_conchodytes_pins(void)
{
    printf("\n-- Conchodytes pinout (netlist contract 2026-08-25) --\n");
    test_conch_sensor_pins();
    test_conch_radio_pins();
    test_conch_click_pairs();
    test_conch_wheel_pins();
    test_conch_battery_pin();
    test_conch_no_forbidden_gpio();
    test_conch_no_reserved_gpio();
    test_conch_no_pin_used_twice();
}
