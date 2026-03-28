/* HID transport abstraction: routes reports to USB or BLE.
   Reusable in any ESP32 HID project using TinyUSB + ESP BLE HID. */
#pragma once

#include <stdint.h>

/* Send a combined keyboard+mouse HID report via the active transport.
   Handles BLE initialization check and automatic fallback to USB. */
void hid_send_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Send a keyboard-only HID report via the active transport. */
void hid_send_keyboard(uint8_t modifier, const uint8_t kb[6]);

/* Send a mouse-only HID report via the active transport. */
void hid_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
