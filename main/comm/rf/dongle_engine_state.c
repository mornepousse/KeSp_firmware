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

/*
 * On keyboards layer_changed() flags the display task.
 * On the dongle: push EN_INFO_LAYER + EN_INFO_STATE to both halves via ESP-NOW.
 */
#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_link.h"
#include "espnow_msg.h"
#endif

void layer_changed(void)
{
#if CONFIG_KASE_HAS_ESPNOW
    /* TODO STUB: push EN_INFO_LAYER + EN_INFO_STATE to halves.
     *
     * When MAC pairing is implemented (Plan 4), load mac_left / mac_right from
     * NVS rf.mac_left / rf.mac_right and call espnow_send() for each.
     *
     * en_layer_t l = {
     *     .layer_idx = current_layout,
     * };
     * strncpy(l.name, get_layer_name(current_layout), 16);
     * espnow_send(mac_left,  EN_INFO_LAYER, &l, sizeof(l));
     * espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof(l));
     *
     * en_state_t s = {
     *     .modifiers = current_modifiers,   // from key_processor.c state
     *     .flags     = 0,                   // TODO: caps_word, bt_connected, usb_active
     * };
     * espnow_send(mac_left,  EN_INFO_STATE, &s, sizeof(s));
     * espnow_send(mac_right, EN_INFO_STATE, &s, sizeof(s));
     *
     * For now: no-op (MACs not configured, espnow_send would fail gracefully). */
#endif /* CONFIG_KASE_HAS_ESPNOW */
}

/* matrix test mode is keyboard-only; inert globals so CDC handlers link */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
