#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

extern void init_hid_bluetooth(void);
extern void deinit_hid_bluetooth(void);
extern void send_hid_bl_key(uint8_t keycodes[6]);
extern bool hid_bluetooth_is_initialized(void);
extern bool hid_bluetooth_is_connected(void);
