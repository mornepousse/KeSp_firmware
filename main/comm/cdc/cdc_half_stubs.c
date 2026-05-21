/*
 * Stubs for symbols that cdc_binary_cmds.c references but are not compiled in
 * half role (Tama, BLE, status display, board layout JSON).
 *
 * Compiled only when CONFIG_KASE_DEVICE_ROLE_HALF=y. Provides no-op
 * implementations so the link succeeds. The corresponding KS_CMD_* commands
 * will be effectively inert on the half (return defaults / do nothing).
 *
 * Engine input-state globals (current_layout, matrix_test_*) are provided
 * by dongle_engine_state.c — the half compiles and links the engine but
 * never calls it at runtime.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Engine input-state globals ─────────────────────────────── *
 * Defined in matrix_scan.c (keyboard) or dongle_engine_state.c (dongle).
 * Neither is compiled for half — provide here so the always-compiled
 * engine files (key_processor.c, key_features.c, hid_report.c) link. */
#include "keyboard_config.h"   /* MATRIX_ROWS, MATRIX_COLS */

#define MAX_REPORT_KEYS 6

uint8_t current_layout = 0;
uint8_t last_layer = 0;
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
volatile uint8_t stat_matrix_changed = 0;
volatile uint8_t is_layer_changed = 0;

/* HID output routing: 0 = USB (half has no BLE). */
uint8_t usb_bl_state = 0;

/* layer_changed: display task hook — no display on half. */
void layer_changed(void) { }

/* matrix test mode: inert on half */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;

/* ── status_display ─────────────────────────────────────────── */
void status_display_update_layer_name(void) { /* no display on half */ }

/* ── board_layout_json ──────────────────────────────────────── *
 * cdc_binary_cmds.c does: extern const char board_layout_json[];
 *                         strlen(board_layout_json);
 * Provide an empty JSON array. */
const char board_layout_json[] = "[]";

/* ── Tamagotchi ─────────────────────────────────────────────── *
 * Forward-declare tama types as opaque. The handlers that use them
 * always check NULL or treat as enum. */
typedef int tama2_action_t;
typedef int tama2_state_t;
typedef struct tama2_stats_s tama2_stats_t;

void tama_engine_save(void) { }
void tama_engine_action(tama2_action_t action) { (void)action; }
tama2_state_t tama_engine_get_state(void) { return 0; }
const tama2_stats_t *tama_engine_get_stats(void) { return NULL; }
bool tama_engine_is_enabled(void) { return false; }
void tama_engine_set_enabled(bool enabled) { (void)enabled; }

/* ── Bluetooth slot management ─────────────────────────────── */
typedef struct bt_device_slot_s bt_device_slot_t;

uint8_t bt_get_active_slot(void) { return 0; }
void bt_switch_slot(uint8_t slot) { (void)slot; }
void bt_next_device(void) { }
void bt_prev_device(void) { }
void bt_start_pairing(void) { }
void bt_disconnect(void) { }
const bt_device_slot_t *bt_get_slot(uint8_t slot) { (void)slot; return NULL; }

/* ── BLE HID status ────────────────────────────────────────── */
bool hid_bluetooth_is_initialized(void) { return false; }
bool hid_bluetooth_is_connected(void) { return false; }
bool hid_bluetooth_is_pairing(void) { return false; }

/* ── BLE HID send + io mode (engine/hid_transport reference these) ── */
void send_hid_bl_key(uint8_t modifier, const uint8_t keycodes[6])
{ (void)modifier; (void)keycodes; }       /* no BLE on half */
void send_hid_bl_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{ (void)buttons; (void)x; (void)y; (void)wheel; }
void save_io_mode(uint8_t mode) { (void)mode; }
