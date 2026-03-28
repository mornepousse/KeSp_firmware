#pragma once
#include <stdlib.h>

#include <stdint.h>


extern uint16_t extra_keycodes[6];
extern void send_hid_key();
extern void send_mouse_report(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
extern void vTaskKeyboard(void *pvParameters);
void keyboard_manager_init();
uint8_t keyboard_get_usb_bl_state(void);

