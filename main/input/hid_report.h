/* HID report queue: serializes keyboard/mouse reports via a FreeRTOS queue.
   The sender task dequeues and dispatches to USB or BLE via hid_transport. */
#pragma once

#include <stdint.h>

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
