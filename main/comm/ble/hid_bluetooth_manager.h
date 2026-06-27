#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#ifndef TEST_HOST
#include "sdkconfig.h"   /* CONFIG_KASE_HAS_BLE (firmware build only) */
#endif

/* ── Multi-device management types (always available) ─────────────── */

#define BT_MAX_DEVICES 3
#define BT_DEVICE_NAME_MAX 20

typedef struct {
    uint8_t addr[6];                     /* BD address */
    char name[BT_DEVICE_NAME_MAX];       /* friendly name (from GAP or user) */
    bool valid;                          /* true if slot has a bonded device */
} bt_device_slot_t;

/* Host tests (TEST_HOST) keep extern decls + provide their own mocks. Firmware
 * keeps them only when BLE is compiled in; otherwise inline no-op stubs below. */
#if defined(TEST_HOST) || CONFIG_KASE_HAS_BLE

/* ── Core BLE HID ────────────────────────────────────────────────── */

void init_hid_bluetooth(void);
void deinit_hid_bluetooth(void);
void send_hid_bl_key(uint8_t modifier, const uint8_t keycodes[6]);
void send_hid_bl_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
bool hid_bluetooth_is_initialized(void);
bool hid_bluetooth_is_connected(void);
void save_bt_state(bool enabled);
bool load_bt_state(void);

/* HID output mode persistence: 0 = USB, 1 = BLE */
void save_io_mode(uint8_t mode);
uint8_t load_io_mode(void);

/* ── Multi-device management ─────────────────────────────────────── */

/* Get current active slot (0-2) */
uint8_t bt_get_active_slot(void);

/* Switch to a specific slot (0-2). Disconnects current, reconnects to target. */
void bt_switch_slot(uint8_t slot);

/* Switch to next/previous slot */
void bt_next_device(void);
void bt_prev_device(void);

/* Enter pairing mode (undirected advertising for new device) */
void bt_start_pairing(void);

/* Check if currently in pairing mode */
bool hid_bluetooth_is_pairing(void);

/* Disconnect current device */
void bt_disconnect(void);

/* Get device info for a slot (NULL if invalid) */
const bt_device_slot_t *bt_get_slot(uint8_t slot);

/* Get connected device name (empty string if not connected) */
const char *bt_get_connected_name(void);

/* Save/load multi-device config to NVS */
void bt_devices_save(void);
void bt_devices_load(void);

#else  /* !CONFIG_KASE_HAS_BLE — wireless-relay / dongle build: BLE compiled out.
        * No-op inline stubs so call sites (cdc, hid_transport, oled, key_processor,
        * keyboard_actions) build unchanged without linking the BLE .c files. */

static inline void init_hid_bluetooth(void) {}
static inline void deinit_hid_bluetooth(void) {}
static inline void send_hid_bl_key(uint8_t m, const uint8_t k[6]) { (void)m; (void)k; }
static inline void send_hid_bl_mouse(uint8_t b, int8_t x, int8_t y, int8_t w) { (void)b; (void)x; (void)y; (void)w; }
static inline bool hid_bluetooth_is_initialized(void) { return false; }
static inline bool hid_bluetooth_is_connected(void) { return false; }
static inline void save_bt_state(bool e) { (void)e; }
static inline bool load_bt_state(void) { return false; }
static inline void save_io_mode(uint8_t m) { (void)m; }
static inline uint8_t load_io_mode(void) { return 0; }
static inline uint8_t bt_get_active_slot(void) { return 0; }
static inline void bt_switch_slot(uint8_t s) { (void)s; }
static inline void bt_next_device(void) {}
static inline void bt_prev_device(void) {}
static inline void bt_start_pairing(void) {}
static inline bool hid_bluetooth_is_pairing(void) { return false; }
static inline void bt_disconnect(void) {}
static inline const bt_device_slot_t *bt_get_slot(uint8_t s) { (void)s; return 0; }
static inline const char *bt_get_connected_name(void) { return ""; }
static inline void bt_devices_save(void) {}
static inline void bt_devices_load(void) {}

#endif /* CONFIG_KASE_HAS_BLE */
