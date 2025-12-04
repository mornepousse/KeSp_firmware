idf_component_register(
    SRCS 
        "main.c"
        "app/dfu_manager.c"
        "app/littlefs_manager.c"
        "display/status_display.c"
        "display/i2c_oled_display.c"
        "display/img_usb.c"
        "display/img_signal.c"
        "display/img_bluetooth.c"
        "comm/cdc_acm_com.c"
        "comm/usb_descriptors.c"
        "comm/esp_hidd_prf_api.c"
        "comm/hid_dev.c"
        "comm/hid_device_le_prf.c"
        "comm/hid_bluetooth_manager.c"
        "input/keyboard_manager.c"
        "input/keymap.c"
        "input/matrix.c"
        "input/ICSH044A.c"
    INCLUDE_DIRS "app" "display" "input" "comm" "include"
    PRIV_REQUIRES esp_wifi esp_driver_gpio esp_timer bt esp_hid nvs_flash esp_lcd unity esp_driver_i2c )
littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
