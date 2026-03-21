#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "display_types.h"
#include "driver/spi_master.h"

#include "version.h"

#define MAX_BT_DEVICENAME_LENGTH 40

#define MASTER  // undefine if you are not flashing the main controller

#define COL2ROW //COL2ROW ROW2COL

#define DEBOUNCE 1 //debounce time in ms

//Define matrix
#define KEYPADS 1 // intended in order to create a Multiple keypad split boards
// MATRIX_ROWS and MATRIX_COLS are now defined in board.h

#define NKRO // does not work on Android and iOS!,  we can get 18KRO on those
#define LAYERS 10 // number of layers defined

//deep sleep parameters, mind that reconnecting after deep sleep might take a minute or two
#define SLEEP_MINS 45 // undefine if you do not need deep sleep, otherwise define number of minutes for deepsleep

/*
 *---------------------------- Everything below here should not be modified for standard usage----------------------
 *
 * */
#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))
#define SET_BIT(var,pos) (var |= 1LLU << pos);

#define MAX_LAYER (LAYERS-1)
#define MOD_LED_BYTES 2 //bytes for led status and modifiers
#define KEYMAP_COLS MATRIX_COLS
#define REPORT_LEN (MOD_LED_BYTES+MATRIX_ROWS*KEYMAP_COLS) //size of hid reports with NKRO and room for 3 key
#define REPORT_COUNT_BYTES (MATRIX_ROWS*KEYMAP_COLS)

#define PLUGIN_BASE_VAL 0x135
#define LAYER_HOLD_MAX_VAL 0x134
#define LAYER_HOLD_BASE_VAL 0x123
#define LAYERS_BASE_VAL 0xFF

typedef struct config_data {
	char bt_device_name[MAX_BT_DEVICENAME_LENGTH];
} config_data_t;

extern uint8_t current_layout;

#define MAX_LAYOUT_NAME_LENGTH 15
// array to hold names of layouts for oled
extern char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];

extern TaskHandle_t xKeyreportTask;

/* GPIOs (ROWS0-4, COLS0-12) are now defined in board.h */

static inline display_hw_config_t keyboard_get_display_config(void)
{
#ifdef BOARD_DISPLAY_BACKEND_ROUND
    display_hw_config_t cfg = {
        .bus_type = BOARD_DISPLAY_BUS,
        .width = BOARD_DISPLAY_WIDTH,
        .height = BOARD_DISPLAY_HEIGHT,
        .pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
        .reset_pin = BOARD_DISPLAY_RESET,
        .spi = {
            .host = BOARD_DISPLAY_SPI_HOST,
            .sclk = BOARD_DISPLAY_SPI_SCLK,
            .mosi = BOARD_DISPLAY_SPI_MOSI,
            .cs = BOARD_DISPLAY_SPI_CS,
            .dc = BOARD_DISPLAY_SPI_DC,
            .backlight = BOARD_DISPLAY_SPI_BL,
        },
    };
#else
	display_hw_config_t cfg = {
		.bus_type = BOARD_DISPLAY_BUS,
		.width = BOARD_DISPLAY_WIDTH,
		.height = BOARD_DISPLAY_HEIGHT,
		.pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
		.reset_pin = BOARD_DISPLAY_RESET,
		.i2c = {
			.host = BOARD_DISPLAY_I2C_HOST,
			.sda = BOARD_DISPLAY_I2C_SDA,
			.scl = BOARD_DISPLAY_I2C_SCL,
			.address = BOARD_DISPLAY_I2C_ADDR,
			.enable_internal_pullups = BOARD_DISPLAY_I2C_PULLUPS,
		},
	};
#endif
	return cfg;
}
