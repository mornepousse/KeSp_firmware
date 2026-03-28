/* HID transport abstraction: USB/BLE routing with automatic fallback */
#include "hid_transport.h"
#include "tinyusb.h"
#include "hid_bluetooth_manager.h"

/* Report IDs - must match usb_hid.c */
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2

/* Transport selector: 0 = USB, 1 = BLE (defined in keyboard_manager.c) */
extern uint8_t usb_bl_state;

void hid_send_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
	if (usb_bl_state == 0) {
		if (tud_hid_ready()) {
			tud_hid_kb_mouse_report(REPORT_ID_KEYBOARD, REPORT_ID_MOUSE,
			                        modifier, kb, buttons, x, y, wheel, 0);
		}
	} else {
		if (hid_bluetooth_is_initialized()) {
			send_hid_bl_key(modifier, kb);
			send_hid_bl_mouse(buttons, x, y, wheel);
		} else {
			usb_bl_state = 0;
			if (tud_hid_ready()) {
				tud_hid_kb_mouse_report(REPORT_ID_KEYBOARD, REPORT_ID_MOUSE,
				                        modifier, kb, buttons, x, y, wheel, 0);
			}
		}
	}
}

void hid_send_keyboard(uint8_t modifier, const uint8_t kb[6])
{
	if (usb_bl_state == 0) {
		if (tud_hid_ready()) {
			tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, kb);
		}
	} else {
		if (hid_bluetooth_is_initialized()) {
			send_hid_bl_key(modifier, kb);
		} else {
			usb_bl_state = 0;
			if (tud_hid_ready()) {
				tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, kb);
			}
		}
	}
}

void hid_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
	if (usb_bl_state == 0) {
		if (tud_hid_ready()) {
			tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
		}
	} else {
		if (hid_bluetooth_is_initialized()) {
			send_hid_bl_mouse(buttons, x, y, wheel);
		} else {
			usb_bl_state = 0;
			if (tud_hid_ready()) {
				tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
			}
		}
	}
}
