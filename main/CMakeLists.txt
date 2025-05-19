idf_component_register(
    SRCS "graphic_manager.c" "display_manager.c" "keyboard_manager.c" "main.c" "keymap.c" "matrix.c" "esp_hidd_prf_api.c" "hid_dev.c" "hid_device_le_prf.c" "hid_bluetooth_manager.c" "ICSH044A.c" 
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_driver_gpio esp_timer bt esp_hid nvs_flash spiffs esp_lcd unity esp_driver_i2c )
