/* LED animation math tests — VRAI module linké (../main/led/led_curve.c).
 * Fini les #define recopiés + formules inline : les constantes, la courbe
 * réactive et le mapping KPM bar testés sont ceux de la prod (led_curve.h/.c). */
#include "test_framework.h"
#include "led_curve.h"

/* Constantes cohérentes (contre les vraies valeurs de led_curve.h). */
void test_reactive_timing(void) {
    TEST_ASSERT(REACTIVE_ATTACK_MS > 0, "attack > 0");
    TEST_ASSERT(REACTIVE_DECAY_MS > REACTIVE_ATTACK_MS, "decay > attack");
    TEST_ASSERT(REACTIVE_DECAY_MS <= 2000, "decay <= 2s (raisonnable)");
}

/* Courbe réactive RÉELLE aux moments clés. */
void test_reactive_brightness_curve(void) {
    TEST_ASSERT_EQ(led_reactive_brightness(0), 255, "t=0 → plein éclat");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_ATTACK_MS - 1), 255, "fin d'attaque → plein");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_ATTACK_MS), 255, "t=attack → 255 (début décroissance)");
    /* Milieu de la décroissance → ~127. */
    uint32_t mid = REACTIVE_ATTACK_MS + (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS) / 2;
    uint8_t b_mid = led_reactive_brightness(mid);
    TEST_ASSERT(b_mid > 120 && b_mid < 135, "milieu décroissance ≈ 127");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_DECAY_MS), 0, "t=decay → éteint");
    TEST_ASSERT_EQ(led_reactive_brightness(REACTIVE_DECAY_MS + 1000), 0, "au-delà de decay → éteint");
}

/* KPM bar RÉEL : mapping linéaire + clamp. */
void test_kpm_bar_mapping(void) {
    const uint8_t n = 10;
    TEST_ASSERT_EQ(led_kpm_bar_lit(0, n), 0, "0 KPM → 0 LED");
    TEST_ASSERT_EQ(led_kpm_bar_lit(KPM_BAR_MAX, n), n, "KPM max → toutes les LEDs");
    TEST_ASSERT_EQ(led_kpm_bar_lit(KPM_BAR_MAX / 2, n), n / 2, "moitié KPM → moitié des LEDs");
    TEST_ASSERT_EQ(led_kpm_bar_lit(KPM_BAR_MAX * 2, n), n, "au-delà du max → clampé à n");
}

/* Frame rate raisonnable. */
void test_frame_rate(void) {
    TEST_ASSERT(LED_STRIP_FRAME_MS > 0, "période de frame > 0");
    int fps = 1000 / LED_STRIP_FRAME_MS;
    TEST_ASSERT(fps >= 25 && fps <= 100, "FPS dans [25, 100]");
}

void test_kpm_bar_max(void) {
    TEST_ASSERT(KPM_BAR_MAX > 0, "KPM_BAR_MAX > 0");
    TEST_ASSERT(KPM_BAR_MAX <= 1000, "KPM_BAR_MAX <= 1000 (raisonnable)");
}

void test_led_anim_constants(void) {
    TEST_SUITE("LED Animation — module réel");
    TEST_RUN(test_reactive_timing);
    TEST_RUN(test_reactive_brightness_curve);
    TEST_RUN(test_kpm_bar_mapping);
    TEST_RUN(test_frame_rate);
    TEST_RUN(test_kpm_bar_max);
}
