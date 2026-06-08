cmake_minimum_required(VERSION 3.16)
project(kase_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -DTEST_HOST")

add_executable(test_runner
    test_ghost_filter.c
    test_key_stats.c
    test_bigram.c
    test_cdc_protocol.c
    test_board_config.c
    test_matrix_constants.c
    test_led_anim_constants.c
    test_keymap_nvs.c
    test_keycodes.c
    test_key_features.c
    test_tap_hold.c
    test_macro_seq.c
    test_tap_dance.c
    test_combo.c
    test_leader.c
    test_tama_engine.c
    test_rf_packet.c
    test_heartbeat.c
    ../main/comm/rf/heartbeat.c
    test_half_matrix_diff.c
    ../main/comm/rf/half_scan_task.c
    test_espnow_msg.c
    test_eink_pack.c
    ../main/periph/eink/eink.c
    test_trackpad_map.c
    test_trackpad_cfg.c
    test_trackpad_core.c
    ../main/periph/trackpad/trackpad_map.c
    ../main/periph/trackpad/trackpad.c
    test_rf_pairing.c
    ../main/comm/rf/rf_pairing.c
    test_espnow_peer_filter.c
    ../main/comm/espnow/espnow_link.c
    test_en_status.c
    test_rf_signal_q255.c
    ../main/comm/rf/rf_rx_task.c
    test_half_power.c
    ../main/comm/rf/half_power.c
    test_half_battery.c
    ../main/comm/rf/half_battery.c
    test_cdc_monitor.c
    ../main/comm/cdc/ks_monitor.c
    test_cdc_rx_feed.c
    ../main/comm/cdc/cdc_binary_protocol.c
    test_keycode_report.c
    ../main/input/key_processor.c
    test_main.c
)

target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/kase_v2_debug
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/periph/eink
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/periph/trackpad
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/rf
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/espnow
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/input
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/ble
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tama
)
