/* Host-side unit test runner for KeSp firmware */
#include "test_framework.h"

/* Shared test counters */
int _test_pass_count = 0;
int _test_fail_count = 0;

/* Test suite declarations */
extern void test_position_mapping(void);
extern void test_ghost_filter(void);
extern void test_key_stats(void);
extern void test_bigram(void);
extern void test_cdc_protocol(void);
extern void test_board_config(void);
extern void test_matrix_constants(void);
extern void test_led_anim_constants(void);

int main(void) {
    printf("KeSp Firmware Unit Tests\n");
    printf("========================\n");

    test_position_mapping();
    test_ghost_filter();
    test_key_stats();
    test_bigram();
    test_cdc_protocol();
    test_board_config();
    test_matrix_constants();
    test_led_anim_constants();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", _test_pass_count, _test_fail_count);
    printf("========================================\n");
    return _test_fail_count > 0 ? 1 : 0;
}
