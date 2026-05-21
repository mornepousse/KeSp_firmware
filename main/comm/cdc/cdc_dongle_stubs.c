/*
 * Stubs for symbols that cdc_binary_cmds.c references but are not compiled in
 * dongle role (Tama, BLE, status display, board layout JSON).
 *
 * Compiled only when CONFIG_KASE_DEVICE_ROLE_DONGLE=y. Provides no-op
 * implementations so the link succeeds. The corresponding KS_CMD_* commands
 * will be effectively inert on the dongle (return defaults / do nothing).
 *
 * When Plan 4/5 add proper dongle CDC commands, they will live in
 * cdc_dongle_cmds.c (separate from this stubs file).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Engine input-state globals (current_layout, matrix_test_*) live in
 * comm/rf/dongle_engine_state.c (Plan 2). */

/* ── status_display ─────────────────────────────────────────── */
void status_display_update_layer_name(void) { /* no display on dongle */ }

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
{ (void)modifier; (void)keycodes; }       /* no BLE on dongle */
void send_hid_bl_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{ (void)buttons; (void)x; (void)y; (void)wheel; }
void save_io_mode(uint8_t mode) { (void)mode; }
