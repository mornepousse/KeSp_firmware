/* Keyboard configuration constants */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_BT_DEVICENAME_LENGTH 40

/* NVS namespace shared by all subsystems */
#define STORAGE_NAMESPACE "storage"

#define NKRO
#define LAYERS 10

#define CHECK_BIT(var,pos) ((var) & (1<<(pos)))
#define SET_BIT(var,pos) (var |= 1LLU << pos);

#define MAX_LAYER (LAYERS-1)
#define MOD_LED_BYTES 2
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
/* Columns covered by the KEYMAP, to be distinguished from MATRIX_COLS which
 * counts the ones the board SCANS. They differ on the master half of a split:
 * the Niphargus left half scans 7 columns but carries the keycodes for all 14,
 * the right half being just a scanner with no keymap engine. A board.h can
 * therefore override it; by default the two notions coincide. */
#ifndef KEYMAP_COLS
#define KEYMAP_COLS MATRIX_COLS
#endif
#define REPORT_LEN (MOD_LED_BYTES+MATRIX_ROWS*KEYMAP_COLS)
#define REPORT_COUNT_BYTES (MATRIX_ROWS*KEYMAP_COLS)
#endif

#define PLUGIN_BASE_VAL 0x135
#define LAYER_HOLD_MAX_VAL 0x134
#define LAYER_HOLD_BASE_VAL 0x123
#define LAYERS_BASE_VAL 0xFF

typedef struct config_data {
	char bt_device_name[MAX_BT_DEVICENAME_LENGTH];
} config_data_t;

extern uint8_t current_layout;

#define MAX_LAYOUT_NAME_LENGTH 15
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
extern char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH];
#endif
