#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_BLUETOOTH_16PX
#define LV_ATTRIBUTE_IMG_BLUETOOTH_16PX
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_BLUETOOTH_16PX uint8_t bluetooth_16px_map[] = {
  0x00, 0x00, 
  0x00, 0x00, 
  0x01, 0xc0, 
  0x01, 0xe0, 
  0x09, 0xb0, 
  0x05, 0x98, 
  0x03, 0xe0, 
  0x01, 0xc0, 
  0x01, 0xc0, 
  0x01, 0xe0, 
  0x07, 0xb0, 
  0x0d, 0x98, 
  0x09, 0xe0, 
  0x01, 0xc0, 
  0x00, 0x80, 
  0x00, 0x00, 
};

const lv_img_dsc_t bluetooth_16px = {
  .header.cf = LV_IMG_CF_ALPHA_1BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 16,
  .header.h = 16,
    .data_size = 32,
    .data = bluetooth_16px_map,
};
