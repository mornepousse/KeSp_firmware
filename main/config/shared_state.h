#pragma once

/*
 * Shared extern declarations used across multiple .c files.
 * Consolidates scattered extern statements into one header.
 */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* From keyboard_manager.c */
extern uint8_t usb_bl_state;

/* From matrix.c */
extern TaskHandle_t keyboard_task_handle;
extern uint8_t current_layout;
extern uint8_t last_layer;
extern bool request_wake_request;

/* From round_ui.c */
extern uint32_t current_kpm;

/* From i2c_oled_display.c / round_display.c */
extern bool display_available;

/* From board_layout.c */
extern const char board_layout_json[];
