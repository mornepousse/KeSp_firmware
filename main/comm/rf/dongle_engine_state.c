/*
 * Engine input-state globals for the dongle.
 * On keyboards these live in matrix_scan.c (not compiled on the dongle).
 * rf_rx_task.c populates current_press_row/col[] + MATRIX_STATE from RF;
 * the engine (build_keycode_report) consumes them exactly as on a keyboard.
 */
#include <stdint.h>
#include <stdbool.h>
#include "keyboard_config.h"   /* MATRIX_ROWS, MATRIX_COLS */

#define MAX_REPORT_KEYS 6      /* must match matrix_scan.c boot-protocol limit */

uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];  /* symbol parity; unused on dongle */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
volatile uint8_t stat_matrix_changed;

uint8_t current_layout = 0;
uint8_t last_layer = 0;

/* HID output routing: 0 = USB (dongle is USB-only, always 0). */
uint8_t usb_bl_state = 0;

/* On keyboards layer_changed() flags the display task; no display on dongle. */
void layer_changed(void) { }

/* matrix test mode is keyboard-only; inert globals so CDC handlers link */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
