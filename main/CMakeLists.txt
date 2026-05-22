# Always-compiled core sources
set(srcs
    "main.c"
    "sys/cpu_time.c"
    "sys/nvs_utils.c"
    "app/dfu_manager.c"
    "app/littlefs_manager.c"

    # USB (always present : HID + CDC)
    "comm/usb/usb_hid.c"
    "comm/hid_transport.c"
    "comm/cdc/cdc_acm_com.c"
    "comm/cdc/cdc_ota.c"
    "comm/cdc/cdc_binary_protocol.c"
    "comm/cdc/cdc_binary_cmds.c"

    # Engine (always compiled — runs on both keyboard and dongle)
    "input/key_processor.c"
    "input/tap_hold.c"
    "input/tap_dance.c"
    "input/combo.c"
    "input/leader.c"
    "input/key_features.c"
    "input/hid_report.c"
    "input/keymap.c"
    "input/key_stats.c"
    "input/keyboard_actions.c"

    # Board-specific keymap + layout (always provided per board)
    "${BOARD_DIR}/board_keymap.c"
    "${BOARD_DIR}/board_layout.c"
)

# Local matrix scan + keyboard task — keyboard role only
if(CONFIG_KASE_HAS_LOCAL_MATRIX)
    list(APPEND srcs
        "input/matrix_scan.c"
        "input/keyboard_task.c"
    )
endif()

# Display backend + status display — keyboard role only
if(CONFIG_KASE_HAS_DISPLAY)
    if(DISPLAY_BACKEND STREQUAL "round")
        list(APPEND srcs
            "display/round/spi_round_display.c"
            "display/round/round_ui.c"
            "display/round/round_backend.c"
        )
    else()
        list(APPEND srcs
            "display/oled/i2c_oled_display.c"
            "display/oled/oled_backend.c"
        )
    endif()
    list(APPEND srcs
        "display/status_display.c"
        "display/assets/img_usb.c"
        "display/assets/img_signal.c"
        "display/assets/img_bluetooth.c"
        "led/led_strip_anim.c"
    )
endif()

# BLE HID — keyboard role only
if(CONFIG_KASE_HAS_BLE)
    list(APPEND srcs
        "comm/ble/esp_hidd_prf_api.c"
        "comm/ble/hid_dev.c"
        "comm/ble/hid_device_le_prf.c"
        "comm/ble/hid_bluetooth_manager.c"
    )
endif()

# Tama virtual pet — keyboard role only
if(CONFIG_KASE_HAS_TAMA)
    list(APPEND srcs
        "tama/tama_engine.c"
        "tama/tama_render.c"
    )
endif()

# Dongle CDC stubs (no-op symbols for tama/bt/display referenced by cdc_binary_cmds.c)
if(CONFIG_KASE_DEVICE_ROLE_DONGLE)
    list(APPEND srcs "comm/cdc/cdc_dongle_stubs.c")
endif()

# CDC stubs for half role (no-op symbols for tama/bt/display/layout)
if(CONFIG_KASE_DEVICE_ROLE_HALF)
    list(APPEND srcs "comm/cdc/cdc_half_stubs.c")
endif()

# RF RX (NRF24) — dongle role only
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs
        "comm/rf/rf_driver.c"
        "comm/rf/rf_rx_task.c"
        "comm/rf/heartbeat.c"
        "comm/rf/dongle_engine_state.c"
    )
endif()

# RF TX (NRF24 PTX) + half scan task + shared SPI bus lock — half role only
if(CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs
        "comm/rf/half_scan_task.c"
        "periph/half_spi.c"
    )
    # rf_driver.c is already in the list when KASE_HAS_RF_RX is also set.
    # When building half standalone (no RF_RX), add it here.
    if(NOT CONFIG_KASE_HAS_RF_RX)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()

# Trackpad IQS5xx skeleton — half role only (compiled on both halves; runtime probe)
if(CONFIG_KASE_HAS_TRACKPAD)
    list(APPEND srcs "periph/trackpad/trackpad.c")
endif()

# ESP-NOW cold path — dongle role only (Plan 5 fills these in)
if(CONFIG_KASE_HAS_ESPNOW)
    # Plan 5 will add: comm/espnow/*.c
endif()

# PRIV_REQUIRES — gate on Kconfig flags
set(priv_requires
    esp_driver_gpio
    esp_timer
    nvs_flash
    unity
    esp_driver_i2c
    app_update
)

if(CONFIG_KASE_HAS_BLE)
    list(APPEND priv_requires bt esp_hid)
endif()

if(CONFIG_KASE_HAS_DISPLAY)
    list(APPEND priv_requires esp_lcd)
endif()

if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()

if(CONFIG_KASE_HAS_RF_TX AND NOT CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()

if(CONFIG_KASE_HAS_ESPNOW)
    list(APPEND priv_requires esp_wifi esp_event)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
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
        "comm/rf"
        "tama"
        "input"
        "periph"
        "periph/trackpad"
        "${BOARD_DIR}"
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_KASE_HAS_DISPLAY)
    littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
endif()
