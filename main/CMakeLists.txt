# Select display backend sources based on board
if(DISPLAY_BACKEND STREQUAL "round")
    set(DISPLAY_BACKEND_SRCS
        "display/round/spi_round_display.c"
        "display/round/round_ui.c"
        "display/round/round_backend.c"
    )
else()
    set(DISPLAY_BACKEND_SRCS
        "display/oled/i2c_oled_display.c"
        "display/oled/oled_backend.c"
    )
endif()

idf_component_register(
    SRCS
        "main.c"
        "sys/cpu_time.c"
        "sys/nvs_utils.c"
        "app/dfu_manager.c"
        "app/littlefs_manager.c"

        "display/status_display.c"
        ${DISPLAY_BACKEND_SRCS}
        "led/led_strip_anim.c"
        "display/assets/img_usb.c"
        "display/assets/img_signal.c"
        "display/assets/img_bluetooth.c"

        "comm/usb/usb_hid.c"
        "comm/hid_transport.c"
        "comm/cdc/cdc_acm_com.c"
        "comm/cdc/cdc_keyboard_cmds.c"
        "comm/cdc/cdc_ota.c"
        "comm/ble/esp_hidd_prf_api.c"
        "comm/ble/hid_dev.c"
        "comm/ble/hid_device_le_prf.c"
        "comm/ble/hid_bluetooth_manager.c"

        "input/keyboard_task.c"
        "input/key_processor.c"
        "input/tap_hold.c"
        "input/tap_dance.c"
        "input/combo.c"
        "input/leader.c"
        "input/key_features.c"
        "input/hid_report.c"
        "input/keymap.c"
        "input/matrix_scan.c"
        "input/key_stats.c"
        "input/keyboard_actions.c"
        "tama/tama_engine.c"
        "tama/tama_render.c"

        "${BOARD_DIR}/board_keymap.c"
        "${BOARD_DIR}/board_layout.c"
    INCLUDE_DIRS
        "."
        "config"
        "sys"
        "app"
        "display"
        "display/oled"
        "display/round"
        "led"
        "display/assets"
        "comm"
        "comm/usb"
        "comm/cdc"
        "comm/ble"
        "tama"
        "input"
        "${BOARD_DIR}"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_timer
        bt
        esp_hid
        nvs_flash
        esp_lcd
        unity
        esp_driver_i2c
        app_update
)
littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
