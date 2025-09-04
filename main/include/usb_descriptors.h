#pragma once


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
