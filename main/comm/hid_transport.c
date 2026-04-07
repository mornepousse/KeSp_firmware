/* HID transport abstraction: USB/BLE routing with automatic fallback */
#include "hid_transport.h"
#include "keyboard_task.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "hid_bluetooth_manager.h"
#include <string.h>

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2

/* Combined keyboard+mouse in a single USB transfer to avoid mouse lag. */
static bool hid_kb_mouse_report(uint8_t modifier, const uint8_t keycode[6],
                                uint8_t buttons, int8_t x, int8_t y,
                                int8_t vertical, int8_t horizontal)
{
    hid_keyboard_report_t kbd = { .modifier = modifier };
    if (keycode) memcpy(kbd.keycode, keycode, 6);

    hid_mouse_report_t mouse = {
        .buttons = buttons, .x = x, .y = y,
        .wheel = vertical, .pan = horizontal
    };

    uint8_t buf[CFG_TUD_HID_EP_BUFSIZE];
    uint16_t pos = 0;

    buf[pos++] = REPORT_ID_KEYBOARD;
    memcpy(&buf[pos], &kbd, sizeof(kbd));
    pos += sizeof(kbd);

    buf[pos++] = REPORT_ID_MOUSE;
    memcpy(&buf[pos], &mouse, sizeof(mouse));
    pos += sizeof(mouse);

    return tud_hid_n_report(0, 0, buf, pos);
}

static void send_usb_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                              uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (tud_hid_ready())
        hid_kb_mouse_report(modifier, kb, buttons, x, y, wheel, 0);
}

static void send_usb_keyboard(uint8_t modifier, const uint8_t kb[6])
{
    if (tud_hid_ready())
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, kb);
}

static void send_usb_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (tud_hid_ready())
        tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
}

void hid_send_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (usb_bl_state == 0) {
        send_usb_kb_mouse(modifier, kb, buttons, x, y, wheel);
    } else if (hid_bluetooth_is_initialized()) {
        send_hid_bl_key(modifier, kb);
        send_hid_bl_mouse(buttons, x, y, wheel);
    } else {
        usb_bl_state = 0;
        send_usb_kb_mouse(modifier, kb, buttons, x, y, wheel);
    }
}

void hid_send_keyboard(uint8_t modifier, const uint8_t kb[6])
{
    if (usb_bl_state == 0) {
        send_usb_keyboard(modifier, kb);
    } else if (hid_bluetooth_is_initialized()) {
        send_hid_bl_key(modifier, kb);
    } else {
        usb_bl_state = 0;
        send_usb_keyboard(modifier, kb);
    }
}

void hid_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (usb_bl_state == 0) {
        send_usb_mouse(buttons, x, y, wheel);
    } else if (hid_bluetooth_is_initialized()) {
        send_hid_bl_mouse(buttons, x, y, wheel);
    } else {
        usb_bl_state = 0;
        send_usb_mouse(buttons, x, y, wheel);
    }
}
