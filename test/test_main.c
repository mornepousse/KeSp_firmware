/* Host-side unit test runner for KeSp firmware */
#include "test_framework.h"

/* Shared test counters */
int _test_pass_count = 0;
int _test_fail_count = 0;

/* Test suite declarations */
extern void test_ghost_filter(void);
extern void test_key_stats(void);
extern void test_bigram(void);
extern void test_cdc_protocol(void);
extern void test_board_config(void);
extern void test_matrix_constants(void);
extern void test_led_anim_constants(void);
extern void test_cdc_parsing(void);
extern void test_keymap_nvs(void);
extern void test_keycodes(void);
extern void test_key_features(void);
extern void test_tap_hold(void);
extern void test_macro_seq(void);
extern void test_tap_dance(void);
extern void test_combo(void);
extern void test_leader(void);
extern void test_tama_engine(void);

int main(void) {
    printf("KeSp Firmware Unit Tests\n");
    printf("========================\n");

    test_ghost_filter();
    test_key_stats();
    test_bigram();
    test_cdc_protocol();
    test_board_config();
    test_matrix_constants();
    test_led_anim_constants();
    test_cdc_parsing();
    test_keymap_nvs();
    test_keycodes();
    test_key_features();
    test_tap_hold();
    test_macro_seq();
    test_tap_dance();
    test_combo();
    test_leader();
    test_tama_engine();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", _test_pass_count, _test_fail_count);
    printf("========================================\n");
    return _test_fail_count > 0 ? 1 : 0;
}
