/* HID report queue: serializes keyboard/mouse reports via a FreeRTOS queue.
   The sender task dequeues and dispatches to USB or BLE via hid_transport. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Initialize the HID queue, mutex, and sender task */
void hid_report_init(void);

/* Enqueue a keyboard report (from keycodes[] global) */
void send_hid_key(void);

/* Enqueue a mouse report */
void send_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Enqueue a combined keyboard+mouse report */
void send_hid_kb_mouse(uint8_t modifier, const uint8_t keycodes[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Get current USB/BLE transport state (0=USB, 1=BLE) */
uint8_t keyboard_get_usb_bl_state(void);

/* Returns the current HID modifier byte (left/right shift/ctrl/alt/GUI).
 * Same encoding as USB HID boot protocol modifier byte. */
uint8_t hid_report_get_modifiers(void);

/* Types `n` HID keyboard usages (1..16), each as a press report then an
 * all-released report, NO modifier, appended in order to the same queue the
 * engine's own reports go through (the sender task paces them on the USB
 * endpoint, ~1 ms apart). Never blocks the scan (the caller is not the
 * keyboard task) and the caller only for a few ms: all or nothing — false,
 * nothing queued, when the queue has no room for the whole sequence. Used
 * by the chest link to type a visible TOTP code (chest_oath_code_key). The
 * caller wipes its own buffer; the usages are never logged here. */
bool hid_report_type_usages(const uint8_t *usages, uint8_t n);
