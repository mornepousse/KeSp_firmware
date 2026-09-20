/* LED animation math tests — REAL module linked (../main/led/led_curve.c).
 * No more copied #define + inline formulas: the constants, the reactive
 * curve, and the tested KPM bar mapping are the ones from prod (led_curve.h/.c). */
#include "test_framework.h"
#include "led_curve.h"

/* Consistent constants (against the real values in led_curve.h). */
void test_reactive_timing(void) {
    TEST_ASSERT(REACTIVE_ATTACK_MS > 0, "attack > 0");
    TEST_ASSERT(REACTIVE_DECAY_MS > REACTIVE_ATTACK_MS, "decay > attack");
    TEST_ASSERT(REACTIVE_DECAY_MS <= 2000, "decay <= 2s (reasonable)");
}

/* REAL reactive curve at key moments. */
void test_reactive_brightness_curve(void) {
    TEST_ASSERT_EQ(led_reactive_brightness(0), 255, "t=0 → full brightness");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_ATTACK_MS - 1), 255, "end of attack → full");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_ATTACK_MS), 255, "t=attack → 255 (start of decay)");
    /* Middle of the decay → ~127. */
    uint32_t mid = REACTIVE_ATTACK_MS + (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS) / 2;
    uint8_t b_mid = led_reactive_brightness(mid);
    TEST_ASSERT(b_mid > 120 && b_mid < 135, "decay midpoint ≈ 127");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_DECAY_MS), 0, "t=decay → off");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_DECAY_MS + 1000), 0, "beyond decay → off");
}

/* REAL KPM bar: linear mapping + clamp. */
void test_kpm_bar_mapping(void) {
    const uint8_t n = 10;
    TEST_ASSERT_EQ(led_kpm_bar_lit(0, n), 0, "0 KPM → 0 LED");
    TEST_ASSERT_EQ(led_kpm_bar_lit(KPM_BAR_MAX, n), n, "KPM max → all LEDs");
    TEST_ASSERT_EQ(led_kpm_bar_lit(KPM_BAR_MAX / 2, n), n / 2, "half KPM → half the LEDs");
    TEST_ASSERT_EQ(led_kpm_bar_lit(KPM_BAR_MAX * 2, n), n, "beyond max → clamped to n");
}

/* Reasonable frame rate. */
void test_frame_rate(void) {
    TEST_ASSERT(LED_STRIP_FRAME_MS > 0, "frame period > 0");
    int fps = 1000 / LED_STRIP_FRAME_MS;
    TEST_ASSERT(fps >= 25 && fps <= 100, "FPS in [25, 100]");
}

void test_kpm_bar_max(void) {
    TEST_ASSERT(KPM_BAR_MAX > 0, "KPM_BAR_MAX > 0");
    TEST_ASSERT(KPM_BAR_MAX <= 1000, "KPM_BAR_MAX <= 1000 (reasonable)");
}

void test_led_anim_constants(void) {
    TEST_SUITE("LED Animation — real module");
    TEST_RUN(test_reactive_timing);
    TEST_RUN(test_reactive_brightness_curve);
    TEST_RUN(test_kpm_bar_mapping);
    TEST_RUN(test_frame_rate);
    TEST_RUN(test_kpm_bar_max);
}
