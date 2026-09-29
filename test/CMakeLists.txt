cmake_minimum_required(VERSION 3.16)
project(kase_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -DTEST_HOST")

add_executable(test_runner
    test_key_stats.c
    test_bigram.c
    ../main/input/key_stats.c
    test_board_config.c
    test_matrix_constants.c
    test_matrix_flag.c
    test_led_anim_constants.c
    ../main/led/led_curve.c
    test_keymap_nvs.c
    nvs_fake.c
    ../main/input/keymap.c
    test_keycodes.c
    test_key_features.c
    ../main/input/key_features.c
    test_tap_hold.c
    ../main/input/tap_hold.c
    host_clock.c
    test_tap_dance.c
    ../main/input/tap_dance.c
    test_combo.c
    ../main/input/combo.c
    test_leader.c
    ../main/input/leader.c
    test_rf_packet.c
    test_mouse_slot_vectors.c
    test_heartbeat.c
    ../main/comm/rf/heartbeat.c
    test_trackpad_map.c
    test_trackpad_cfg.c
    ../main/periph/trackpad/trackpad_map.c
    test_rf_pairing.c
    ../main/comm/rf/rf_pairing.c
    test_rf_slot.c
    test_rf_signal_q255.c
    ../main/comm/rf/rf_rx_task.c
    test_cdc_monitor.c
    ../main/comm/cdc/ks_monitor.c
    test_cdc_rx_feed.c
    ../main/comm/cdc/cdc_binary_protocol.c
    test_keycode_report.c
    ../main/input/key_processor.c
    ../main/comm/chest/chest_gate.c
    test_sec_confirm.c
    ../main/security/sec_confirm.c
    test_sec_store.c
    ../main/security/sec_store.c
    test_cdc_sec.c
    ../main/comm/cdc/cdc_sec_cmds.c
    test_chest_proto.c
    ../main/comm/chest/chest_proto.c
    test_chest_dma.c
    ../main/comm/chest/chest_dma.c
    test_chest_oath.c
    ../main/comm/chest/chest_oath.c
    test_chest_view.c
    ../main/comm/chest/chest_view.c
    test_chest_round.c
    ../main/comm/chest/chest_round.c
    test_cr_crc16.c
    ../main/security/cr_crc16.c
    test_otp_proto.c
    ../main/security/otp_proto.c
    test_apdu.c
    ../main/security/apdu.c
    test_openpgp_do.c
    ../main/security/openpgp_do.c
    test_openpgp_card.c
    ../main/security/openpgp_card.c
    test_cfg_bridge.c
    ../main/comm/rf/cfg_bridge.c
    test_kbd_route.c
    test_kbd_refresh.c
    test_niphar_keymap_span.c
    test_rf_channel_plan.c
    test_half_state.c
    test_fuse_halves.c
    test_fusion_state.c
    test_config_sync.c
    test_fusion_route.c
    test_matrix_bitmap.c
    test_veille.c
    test_keyboard_cadence.c
    test_cadence.c
    test_veille_veto.c
    test_radio_owner.c
    test_fusion_file.c
    ../main/comm/rf/radio_owner.c
    test_rf_status_cadence.c
    test_keymap_blob_size.c
    test_half_col_map.c
    test_half_tx_cadence.c
    test_half_tx_target.c
    test_half_tx_repeat.c
    test_keymap_sync_frames.c
    test_keymap_sync.c
    test_wake_grace.c
    test_batt_calc.c
    test_memlcd_model.c
    test_oled_nav.c
    ../main/display/oled/oled_nav.c
    test_niphar_left_pins.c
    test_niphar_right_pins.c
    test_board_contract_kase_v1.c
    test_board_contract_kase_v2.c
    test_board_contract_kase_v2_debug.c
    test_board_contract_kase_dongle.c
    test_board_contract_niphar_left.c
    test_board_contract_niphar_right.c
    test_mouse_input.c
    ../main/input/mouse_buttons.c
    ../main/input/mouse_wheel.c
    test_hid_dedup.c
    test_oled_stats.c
    ../main/display/oled/oled_stats.c
    test_link_frame.c
    test_link_handshake.c
    test_main.c
)

target_compile_definitions(test_runner PRIVATE SEC_CONFIRM_LOCAL_COLS=4)

# chest_gate.c's oath-nav CAS loop takes a test-only seam (I1, review
# 2026-09-29) — scoped to this one source file, not the whole target, so no
# other translation unit sees CHEST_GATE_TEST_SEAM.
set_source_files_properties(../main/comm/chest/chest_gate.c PROPERTIES
    COMPILE_DEFINITIONS "CHEST_GATE_TEST_SEAM=chest_gate_test_seam_hook")

target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/kase_v2_debug
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/periph/trackpad
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/rf
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/link
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/chest
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/input
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/power
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/ble
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/led
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/security
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/cdc
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/display/oled
)

# ---------------------------------------------------------------------------
# Sanitized chest test binary (review fix round 1, 2026-09-29).
#
# Some of chest_dma.c's bounds (main/comm/chest/chest_dma.c) guard a REAL
# out-of-bounds stack write (e.g. `count > CHEST_LIST_MAX_ENTRIES`, which
# stops the LIST decoder writing past chest_list_t.e[31]). At -O0, an
# ordinary build can survive that write "by accident": the overflow clobbers
# unrelated stack locals and the function can still happen to return the
# expected value, so a mutation that removes the guard does not reliably
# turn the ordinary test_runner red. Only a sanitizer pins it.
#
# test_chest_sanitized carries ONLY the chest suites (test_chest_proto.c,
# test_chest_dma.c and the sources they need) — deliberately NOT the full
# suite: openpgp has two pre-existing ASan findings, out of scope here, and
# would make this target red for a reason that has nothing to do with the
# chest link.
#
# It is part of the DEFAULT build target (no EXCLUDE_FROM_ALL), and the
# add_custom_command below runs it as a POST_BUILD step on its own target —
# so `cmake --build test/build` itself fails on a red run. scripts/check.sh's
# fast phase is exactly `cmake -S test -B test/build && cmake --build
# test/build && ./test/build/test_runner`: this makes the sanitized binary
# part of that build step WITHOUT any change to check.sh — the build fails,
# and the `&&` chain never reaches test_runner, before scripts/check.sh ever
# has to know this binary exists.
add_executable(test_chest_sanitized
    test_chest_proto.c
    ../main/comm/chest/chest_proto.c
    test_chest_dma.c
    ../main/comm/chest/chest_dma.c
    test_chest_oath.c
    ../main/comm/chest/chest_oath.c
    test_chest_view.c
    ../main/comm/chest/chest_view.c
    test_chest_round.c
    ../main/comm/chest/chest_round.c
    ../main/security/cr_crc16.c
    # test_memlcd_model.c (review M-f, 2026-09-29): pure header-only logic
    # (memlcd_model.h — memlcd_couper_8/memlcd_bas_coffre index caller-sized
    # stack arrays the same way chest_dma.c's LIST decoder does), and all of
    # its OWN dependencies (chest_op_label/chest_mode_label from
    # chest_proto.c, chest_view_build/chest_view_age from chest_view.c) are
    # already sources of this binary — no new ones needed.
    test_memlcd_model.c
    test_main_chest_sanitized.c
)

target_include_directories(test_chest_sanitized PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/comm/chest
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/security
)

target_compile_options(test_chest_sanitized PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(test_chest_sanitized PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)

add_custom_command(TARGET test_chest_sanitized POST_BUILD
    COMMAND $<TARGET_FILE:test_chest_sanitized>
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_chest_sanitized>
    COMMENT "Running test_chest_sanitized (ASan+UBSan) — a red run fails this build step"
    VERBATIM
)
