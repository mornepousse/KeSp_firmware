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

//#define VERSION_1
#define VERSION_2

#define MANUFACTURER_NAME "MornePousse"                // 1: Manufacturer
#define PRODUCT_NAME "KaSeV2"               // 2: Product
#define SERIAL_NUMBER "1994"               // 3: Serials, should use chip ID

#define MODULE_ID "MORNEPOUSSE"
#define GATTS_TAG "MAE V2.0" // The device's name
#define MAX_BT_DEVICENAME_LENGTH 40

#define MASTER  // undefine if you are not flashing the main controller

#define COL2ROW //COL2ROW ROW2COL

#define DEBOUNCE 4 //debounce time in ms

//Define matrix
#define KEYPADS 1 // intended in order to create a Multiple keypad split boards
#define MATRIX_ROWS 5 // For split keyboards, define rows for one side only.
#define MATRIX_COLS 13 // For split keyboards, define columns for one side only.

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


#ifdef VERSION_2 
#define ROWS0 GPIO_NUM_10
#define ROWS1 GPIO_NUM_11
#define ROWS2 GPIO_NUM_12
#define ROWS3 GPIO_NUM_13
#define ROWS4 GPIO_NUM_14


#define COLS0 GPIO_NUM_9
#define COLS1 GPIO_NUM_46
#define COLS2 GPIO_NUM_3
#define COLS3 GPIO_NUM_8
#define COLS4 GPIO_NUM_18
#define COLS5 GPIO_NUM_17
#define COLS6 GPIO_NUM_16
#define COLS7 GPIO_NUM_43
#define COLS8 GPIO_NUM_44
#define COLS9 GPIO_NUM_1
#define COLS10 GPIO_NUM_2
#define COLS11 GPIO_NUM_42
#define COLS12 GPIO_NUM_41
#endif

#ifdef VERSION_1

#define ROWS0 GPIO_NUM_3
#define ROWS1 GPIO_NUM_36
#define ROWS2 GPIO_NUM_39
#define ROWS3 GPIO_NUM_40
#define ROWS4 GPIO_NUM_37


#define COLS0 GPIO_NUM_21
#define COLS1 GPIO_NUM_47
#define COLS2 GPIO_NUM_48
#define COLS3 GPIO_NUM_45
#define COLS4 GPIO_NUM_35
#define COLS5 GPIO_NUM_38
#define COLS6 GPIO_NUM_9
#define COLS7 GPIO_NUM_10
#define COLS8 GPIO_NUM_11
#define COLS9 GPIO_NUM_12
#define COLS10 GPIO_NUM_13
#define COLS11 GPIO_NUM_14
#define COLS12 GPIO_NUM_46

#endif

static inline display_hw_config_t keyboard_get_display_config(void)
{
	display_hw_config_t cfg = {
		.bus_type = DISPLAY_BUS_I2C,
		.width = 128,
		.height = 64,
		.pixel_clock_hz = 400 * 1000,
		.reset_pin = GPIO_NUM_NC,
		.i2c = {
			.host = I2C_NUM_0,
			.sda = GPIO_NUM_15,
			.scl = GPIO_NUM_7,
			.address = 0x3C,
			.enable_internal_pullups = true,
		},
	};
	return cfg;
}