#pragma once
#include <stdint.h>


enum
{
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_VENDOR,
  ITF_NUM_KEYBOARD,
  ITF_NUM_MOUSE,
  ITF_NUM_TOTAL
};
 
void kase_tinyusb_init(void);

/* LED state from host (SET_REPORT): bit0=NumLock, bit1=CapsLock, bit2=ScrollLock */
extern volatile uint8_t hid_led_state;
#define HID_LED_CAPS_LOCK  (1 << 1)
