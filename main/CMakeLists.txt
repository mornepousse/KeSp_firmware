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
    "comm/cdc/ks_monitor.c"

    # Config bridge: dongle↔smart-keyboard KS/KR tunnelling over ESP-NOW.
    # Pure chunk/predicate logic compiles on all boards; the ESP-NOW networking
    # functions are gated inside the file (dongle role / wireless keyboard).
    "comm/rf/cfg_bridge.c"

    # Security confirmation gate — compiled for all roles (key_processor calls it)
    "security/sec_confirm.c"

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

# NRF24 + ESP-NOW radio stack + HID relay TX — keyboard wireless relay mode only
if(CONFIG_KASE_KBD_WIRELESS)
    list(APPEND srcs
        "comm/rf/rf_driver.c"
        "comm/rf/rf_pairing.c"
        "comm/rf/kbd_relay_tx.c"
        "comm/espnow/espnow_link.c"
        "comm/espnow/espnow_info.c")
endif()

# Dongle CDC stubs (no-op symbols for tama/bt/display referenced by cdc_binary_cmds.c)
# and dongle-specific binary commands (RF status, pair list/reset, battery).
# cr_hmac is kept unconditional: cdc_sec_cmds.c KAT self-test references it directly.
# cr_crc16 is kept unconditional: tiny file, linker-stripped under NONE/OPENPGP.
if(CONFIG_KASE_DEVICE_ROLE_DONGLE)
    list(APPEND srcs
        "comm/cdc/cdc_dongle_stubs.c"
        "comm/cdc/cdc_dongle_cmds.c"
        "security/sec_store.c"
        "comm/cdc/cdc_sec_cmds.c"
        "security/cr_crc16.c"
        "security/cr_hmac.c"
    )
    # OTP transport (YubiKey CR-HMAC personality only)
    if(CONFIG_KASE_SEC_OTP_HID)
        list(APPEND srcs
            "security/otp_proto.c"
            "security/otp_hid.c"
        )
    endif()
    # OpenPGP smartcard personality: CCID class driver + applet + crypto
    if(CONFIG_KASE_SEC_OPENPGP)
        list(APPEND srcs
            "security/ccid.c"
            "security/openpgp_card.c"
            "security/apdu.c"
            "security/openpgp_do.c"
            "security/openpgp_crypto.c"
        )
    endif()
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
        # Pure trackpad gesture+accel mapping: the dongle applies it on the raw
        # trackpad packets received from the halves (approach B). The half only
        # forwards raw data, so it does not compile this.
        "periph/trackpad/trackpad_map.c"
        # Runtime accel cfg store + NVS persistence (dongle only).
        "periph/trackpad/trackpad_cfg.c"
    )
endif()

# RF TX (NRF24 PTX) + half scan task + shared SPI bus lock — half role only
if(CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs
        "comm/rf/half_scan_task.c"
        "comm/rf/half_power.c"
        "comm/rf/half_battery.c"
        "comm/rf/half_sleep.c"
        "periph/half_spi.c"
    )
    # rf_driver.c is already in the list when KASE_HAS_RF_RX is also set.
    # When building half standalone (no RF_RX), add it here.
    if(NOT CONFIG_KASE_HAS_RF_RX)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()

# Per-set NRF addressing + pairing (Plan RF-1/RF-2) — dongle (RX) and half (TX)
if(CONFIG_KASE_HAS_RF_RX OR CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs "comm/rf/rf_pairing.c")
endif()

# NRF line short-circuit bring-up diagnostic — only when explicitly enabled
if(CONFIG_KASE_NRF_LINE_TEST)
    list(APPEND srcs "comm/rf/rf_line_test.c")
endif()

# Trackpad IQS5xx skeleton — half role only (compiled on both halves; runtime probe)
if(CONFIG_KASE_HAS_TRACKPAD)
    list(APPEND srcs "periph/trackpad/trackpad.c")
endif()

# E-ink SSD1681 + LVGL monochrome driver — half role only
if(CONFIG_KASE_HAS_EINK)
    list(APPEND srcs
        "periph/eink/eink.c"
        "periph/eink/eink_lvgl.c"
    )
    list(APPEND priv_requires lvgl)   # raw LVGL v8 — managed component
endif()

# ESP-NOW info channel — dongle + half roles (KASE_HAS_ESPNOW=y on both)
if(CONFIG_KASE_HAS_ESPNOW)
    list(APPEND srcs
        "comm/espnow/espnow_link.c"
        "comm/espnow/espnow_info.c"
    )
    # Plan 5 will add: EN_OTA_*, EN_CFG_*, EN_TELEM_* handlers in additional .c files.
endif()

# PRIV_REQUIRES — gate on Kconfig flags
set(priv_requires
    esp_driver_gpio
    esp_timer
    nvs_flash
    nvs_sec_provider  # NVS encryption HMAC scheme (CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC).
                      # Unconditional like esp_wifi/bt above: a conditional require fails on
                      # the first config pass (CONFIG unknown) and the NVS_SEC_* symbols stay
                      # unknown. Compiles to ~nothing when NVS_ENCRYPTION=n.
    unity
    esp_driver_i2c
    app_update
    esp_wifi    # esp_now.h lives in esp_wifi/include; needed when ESPNOW=y.
                # esp_wifi component always registers (even if WiFi stack disabled)
                # so listing it unconditionally is safe and avoids CMake conditional issues.
    esp_event   # esp_event_loop_create_default is called from espnow_link.c
    bt          # BLE HID (keyboard role). MUST be unconditional: with set(COMPONENTS main)
    esp_hid     # a conditional `if(CONFIG_KASE_HAS_BLE)` requires fails to pull bt on a fresh
                # config (CONFIG unknown on first pass) and nothing else pulls it transitively
                # → BT_* Kconfig symbols unknown + esp_bt_defs.h missing. Same fix as esp_wifi.
                # Harmless on dongle/half (BT disabled in their sdkconfig → compiles to ~nothing).
    esp_driver_tsens  # CPU temperature sensor — fastfetch e-ink dashboard (half)
    spi_flash         # esp_flash_get_size / esp_partition_* — dashboard flash stats
    mbedtls           # HMAC-SHA1 for OTP HID (cr_hmac.c, dongle only)
)

if(CONFIG_KASE_HAS_DISPLAY)
    list(APPEND priv_requires esp_lcd)
endif()

if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()

if(CONFIG_KASE_HAS_RF_TX AND NOT CONFIG_KASE_HAS_RF_RX)
    list(APPEND priv_requires esp_driver_spi)
endif()

if(CONFIG_KASE_KBD_WIRELESS AND NOT CONFIG_KASE_HAS_RF_RX AND NOT CONFIG_KASE_HAS_RF_TX)
    list(APPEND priv_requires esp_driver_spi)
endif()

# esp_wifi and esp_event are now unconditionally in priv_requires above.
# This conditional block is kept for documentation/future conditional additions.
if(CONFIG_KASE_HAS_ESPNOW)
    # Placeholder: additional ESPNOW-specific priv_requires can be added here.
    # esp_wifi + esp_event already in base set above.
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "sys"
        "app"
        "security"
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
        "periph/eink"
        "comm/espnow"
        "${BOARD_DIR}"
    PRIV_REQUIRES ${priv_requires}
)

if(CONFIG_KASE_HAS_DISPLAY)
    littlefs_create_partition_image(storage ../flash_data FLASH_IN_PROJECT)
endif()
