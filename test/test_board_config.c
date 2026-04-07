/* Test board configuration consistency */
#include "test_framework.h"

/* Stub GPIO_NUM_* for host tests (ESP-IDF defines these as enums) */
#ifndef GPIO_NUM_0
#define GPIO_NUM_0  0
#define GPIO_NUM_1  1
#define GPIO_NUM_2  2
#define GPIO_NUM_3  3
#define GPIO_NUM_4  4
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
#define GPIO_NUM_35 35
#define GPIO_NUM_36 36
#define GPIO_NUM_37 37
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_43 43
#define GPIO_NUM_44 44
#define GPIO_NUM_45 45
#define GPIO_NUM_46 46
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
#define GPIO_NUM_NC (-1)
#define SPI2_HOST   1
#define I2C_NUM_0   0
#endif

/* Stub display types for host */
#ifndef DISPLAY_BUS_I2C
#define DISPLAY_BUS_I2C 0
#define DISPLAY_BUS_SPI 1
#endif

/* Stub LVGL font for host */
#define lv_font_montserrat_14 0
#define lv_font_montserrat_28 0

#include "board.h"

/* Test: required product info macros are defined and non-empty */
void test_board_product_info(void) {
    TEST_ASSERT(strlen(GATTS_TAG) > 0, "GATTS_TAG defined");
    TEST_ASSERT(strlen(MANUFACTURER_NAME) > 0, "MANUFACTURER_NAME defined");
    TEST_ASSERT(strlen(PRODUCT_NAME) > 0, "PRODUCT_NAME defined");
    TEST_ASSERT(strlen(SERIAL_NUMBER) > 0, "SERIAL_NUMBER defined");
    TEST_ASSERT(MODULE_ID > 0, "MODULE_ID > 0");
}

/* Test: matrix dimensions are valid */
void test_board_matrix_dimensions(void) {
    TEST_ASSERT_EQ(MATRIX_ROWS, 5, "MATRIX_ROWS == 5");
    TEST_ASSERT_EQ(MATRIX_COLS, 13, "MATRIX_COLS == 13");
}

/* Test: GPIO pins are in valid ESP32-S3 range (0-48) */
void test_board_gpio_range(void) {
    int row_gpios[] = {ROWS0, ROWS1, ROWS2, ROWS3, ROWS4};
    int col_gpios[] = {COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                       COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12};

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(row_gpios[i] >= 0 && row_gpios[i] <= 48, "ROW GPIO in range");
    }
    for (int i = 0; i < 13; i++) {
        TEST_ASSERT(col_gpios[i] >= 0 && col_gpios[i] <= 48, "COL GPIO in range");
    }
}

/* Test: no GPIO collisions between rows and cols */
void test_board_gpio_no_collision(void) {
    int all_gpios[] = {ROWS0, ROWS1, ROWS2, ROWS3, ROWS4,
                       COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                       COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12};
    int n = 18;

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (all_gpios[i] == all_gpios[j]) {
                char msg[64];
                snprintf(msg, sizeof(msg), "GPIO collision: pin[%d]=%d == pin[%d]=%d", i, all_gpios[i], j, all_gpios[j]);
                TEST_ASSERT(0, msg);
            } else {
                _test_pass_count++;
            }
        }
    }
}

/* Test: display config macros are consistent */
void test_board_display_config(void) {
    TEST_ASSERT(BOARD_DISPLAY_WIDTH > 0, "BOARD_DISPLAY_WIDTH > 0");
    TEST_ASSERT(BOARD_DISPLAY_HEIGHT > 0, "BOARD_DISPLAY_HEIGHT > 0");
    TEST_ASSERT(BOARD_DISPLAY_CLK_HZ > 0, "BOARD_DISPLAY_CLK_HZ > 0");

    /* Exactly one display backend must be defined */
#if defined(BOARD_DISPLAY_BACKEND_ROUND) && defined(BOARD_DISPLAY_BACKEND_OLED)
    TEST_ASSERT(0, "both ROUND and OLED backends defined");
#elif !defined(BOARD_DISPLAY_BACKEND_ROUND) && !defined(BOARD_DISPLAY_BACKEND_OLED)
    TEST_ASSERT(0, "no display backend defined");
#else
    _test_pass_count++;
#endif
}

/* Test: UI scale and font are defined */
void test_board_ui_config(void) {
    TEST_ASSERT(UI_SCALE >= 1 && UI_SCALE <= 4, "UI_SCALE in [1,4]");
    /* UI_FONT is a pointer, can't easily test at compile time in host tests */
}

/* Test: LED strip config is consistent */
void test_board_led_strip_config(void) {
#if BOARD_HAS_LED_STRIP
    TEST_ASSERT(BOARD_LED_STRIP_GPIO >= 0 && BOARD_LED_STRIP_GPIO <= 48, "LED strip GPIO in range");
    TEST_ASSERT(BOARD_LED_STRIP_NUM_LEDS > 0, "LED strip has LEDs");
#else
    _test_pass_count++;
    _test_pass_count++;
#endif
}

/* Test: feature flags are 0 or 1 */
void test_board_feature_flags(void) {
    TEST_ASSERT(BOARD_HAS_LED_STRIP == 0 || BOARD_HAS_LED_STRIP == 1, "BOARD_HAS_LED_STRIP is boolean");
}

/* Test: matrix delays are non-negative */
void test_board_matrix_delays(void) {
    TEST_ASSERT(BOARD_MATRIX_SETTLING_US >= 0, "settling delay >= 0");
    TEST_ASSERT(BOARD_MATRIX_RECOVERY_US >= 0, "recovery delay >= 0");
}

/* Test: USB VID/PID are defined */
void test_board_usb_ids(void) {
    TEST_ASSERT(BOARD_USB_VID > 0, "BOARD_USB_VID > 0");
    TEST_ASSERT(BOARD_USB_PID > 0, "BOARD_USB_PID > 0");
}

/* Test: debounce ticks are positive */
void test_board_debounce(void) {
    TEST_ASSERT(BOARD_DEBOUNCE_TICKS > 0, "BOARD_DEBOUNCE_TICKS > 0");
}

/* Test: display sleep timeout is reasonable */
void test_board_display_sleep(void) {
    TEST_ASSERT(BOARD_DISPLAY_SLEEP_MS >= 1000, "BOARD_DISPLAY_SLEEP_MS >= 1s");
}

/* Test: matrix scan interval is defined */
void test_board_matrix_scan_interval(void) {
    TEST_ASSERT(BOARD_MATRIX_SCAN_INTERVAL_US > 0, "BOARD_MATRIX_SCAN_INTERVAL_US > 0");
}

/* Test: deep sleep minutes are non-negative */
void test_board_sleep_mins(void) {
    TEST_ASSERT(BOARD_SLEEP_MINS >= 0, "BOARD_SLEEP_MINS >= 0");
}

void test_board_config(void) {
    TEST_SUITE("Board Configuration");
    TEST_RUN(test_board_product_info);
    TEST_RUN(test_board_matrix_dimensions);
    TEST_RUN(test_board_gpio_range);
    TEST_RUN(test_board_gpio_no_collision);
    TEST_RUN(test_board_display_config);
    TEST_RUN(test_board_ui_config);
    TEST_RUN(test_board_led_strip_config);
    TEST_RUN(test_board_feature_flags);
    TEST_RUN(test_board_matrix_delays);
    TEST_RUN(test_board_usb_ids);
    TEST_RUN(test_board_debounce);
    TEST_RUN(test_board_display_sleep);
    TEST_RUN(test_board_matrix_scan_interval);
    TEST_RUN(test_board_sleep_mins);
}
