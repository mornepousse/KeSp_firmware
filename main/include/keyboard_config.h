#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#define MODULE_ID "MORNEPOUSSE"
#define GATTS_TAG "MAE V1.0A" // The device's name
#define MAX_BT_DEVICENAME_LENGTH 40

#define MASTER  // undefine if you are not flashing the main controller

#define COL2ROW //COL2ROW ROW2COL

#define DEBOUNCE 4 //debounce time in ms

//Define matrix
#define KEYPADS 1 // intended in order to create a Multiple keypad split boards
#define MATRIX_ROWS 5 // For split keyboards, define rows for one side only.
#define MATRIX_COLS 13 // For split keyboards, define columns for one side only.

#define NKRO // does not work on Android and iOS!,  we can get 18KRO on those
#define LAYERS 3 // number of layers defined

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
