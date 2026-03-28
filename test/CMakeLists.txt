cmake_minimum_required(VERSION 3.16)
project(kase_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -DTEST_HOST")

add_executable(test_runner
    test_position_mapping.c
    test_ghost_filter.c
    test_key_stats.c
    test_bigram.c
    test_cdc_protocol.c
    test_board_config.c
    test_matrix_constants.c
    test_led_anim_constants.c
    test_cdc_parsing.c
    test_keymap_nvs.c
    test_keycodes.c
    test_key_features.c
    test_tap_hold.c
    test_macro_seq.c
    test_main.c
)

target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../boards/kase_v2_debug
)
