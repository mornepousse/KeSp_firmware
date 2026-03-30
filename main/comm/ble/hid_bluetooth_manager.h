#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

/* ── Core BLE HID ────────────────────────────────────────────────── */

void init_hid_bluetooth(void);
void deinit_hid_bluetooth(void);
void send_hid_bl_key(uint8_t modifier, uint8_t keycodes[6]);
void send_hid_bl_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
bool hid_bluetooth_is_initialized(void);
bool hid_bluetooth_is_connected(void);
void save_bt_state(bool enabled);
bool load_bt_state(void);

/* ── Multi-device management ─────────────────────────────────────── */

#define BT_MAX_DEVICES 3
#define BT_DEVICE_NAME_MAX 20

typedef struct {
    uint8_t addr[6];                     /* BD address */
    char name[BT_DEVICE_NAME_MAX];       /* friendly name (from GAP or user) */
    bool valid;                          /* true if slot has a bonded device */
} bt_device_slot_t;

/* Get current active slot (0-2) */
uint8_t bt_get_active_slot(void);

/* Switch to a specific slot (0-2). Disconnects current, reconnects to target. */
void bt_switch_slot(uint8_t slot);

/* Switch to next/previous slot */
void bt_next_device(void);
void bt_prev_device(void);

/* Enter pairing mode (undirected advertising for new device) */
void bt_start_pairing(void);

/* Disconnect current device */
void bt_disconnect(void);

/* Get device info for a slot (NULL if invalid) */
const bt_device_slot_t *bt_get_slot(uint8_t slot);

/* Get connected device name (empty string if not connected) */
const char *bt_get_connected_name(void);

/* Save/load multi-device config to NVS */
void bt_devices_save(void);
void bt_devices_load(void);
