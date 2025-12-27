#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

extern void init_hid_bluetooth(void);
extern void deinit_hid_bluetooth(void);
extern void send_hid_bl_key(uint8_t modifier, uint8_t keycodes[6]);
extern void send_hid_bl_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
extern bool hid_bluetooth_is_initialized(void);
extern bool hid_bluetooth_is_connected(void);
