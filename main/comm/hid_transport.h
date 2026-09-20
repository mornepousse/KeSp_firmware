/* HID transport abstraction: routes reports to USB or BLE.
   Reusable in any ESP32 HID project using TinyUSB + ESP BLE HID. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Create the USB tx mutex. Call once, before any hid_send_*. */
void hid_transport_init(void);

/* The hid_send_* functions return whether the report actually reached the host.
   Callers MUST NOT record a report as sent on a false return: a report believed
   sent is never retransmitted, and a lost key-up then sticks the key on the host
   (audit F1, docs/CODE_AUDIT_2026-07-07.md). On USB the send waits for a busy
   endpoint rather than dropping, so false means the host is really unreachable. */

/* Send a combined keyboard+mouse HID report via the active transport.
   Handles BLE initialization check and automatic fallback to USB. */
bool hid_send_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Send a keyboard-only HID report via the active transport. */
bool hid_send_keyboard(uint8_t modifier, const uint8_t kb[6]);

/* Send a mouse-only HID report via the active transport. */
bool hid_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Bench counters: USB keyboard reports sent / refused (endpoint gone
 * silent), bus resumes requested / left stuck. See hid_transport.c. */
void hid_transport_stats(uint32_t *kb_ok, uint32_t *kb_refuses, uint32_t *reprises, uint32_t *reprises_ratees);
