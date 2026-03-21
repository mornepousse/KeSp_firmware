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

#ifndef LV_ATTRIBUTE_IMG_WIFI
#define LV_ATTRIBUTE_IMG_WIFI
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_WIFI uint8_t wifi_map[] = {
  0x00, 0x00, 
  0x01, 0x80, 
  0x1f, 0xf8, 
  0x30, 0x0c, 
  0x40, 0x02, 
  0x83, 0xc1, 
  0x0c, 0x70, 
  0x10, 0x08, 
  0x00, 0x00, 
  0x03, 0xc0, 
  0x04, 0x20, 
  0x00, 0x00, 
  0x00, 0x00, 
  0x01, 0x80, 
  0x00, 0x00, 
  0x00, 0x00, 
};

const lv_img_dsc_t wifi = {
  .header.cf = LV_IMG_CF_ALPHA_1BIT,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 16,
  .header.h = 16,
  .data_size = 32,
  .data = wifi_map,
};
