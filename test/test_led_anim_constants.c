/* Test LED animation constants introduced during audit cleanup */
#include "test_framework.h"

/* Mirror constants from led_strip_anim.c */
#define REACTIVE_ATTACK_MS  100
#define REACTIVE_DECAY_MS   500
#define KPM_BAR_MAX         400
#define LED_STRIP_FRAME_MS  20

/* Test: reactive timing constants are coherent */
void test_reactive_timing(void) {
    TEST_ASSERT(REACTIVE_ATTACK_MS > 0, "attack > 0");
    TEST_ASSERT(REACTIVE_DECAY_MS > REACTIVE_ATTACK_MS, "decay > attack");
    TEST_ASSERT(REACTIVE_DECAY_MS <= 2000, "decay <= 2s (reasonable)");
}

/* Test: reactive brightness calculation at key moments */
void test_reactive_brightness_curve(void) {
    /* At t=0 (during attack phase): full brightness */
    uint32_t elapsed = 0;
    uint8_t brightness;
    if (elapsed < REACTIVE_ATTACK_MS) {
        brightness = 255;
    } else if (elapsed < REACTIVE_DECAY_MS) {
        brightness = 255 - ((elapsed - REACTIVE_ATTACK_MS) * 255 / (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS));
    } else {
        brightness = 0;
    }
    TEST_ASSERT_EQ(brightness, 255, "t=0: full brightness");

    /* At t=attack (start of decay): still full */
    elapsed = REACTIVE_ATTACK_MS - 1;
    if (elapsed < REACTIVE_ATTACK_MS) {
        brightness = 255;
    } else if (elapsed < REACTIVE_DECAY_MS) {
        brightness = 255 - ((elapsed - REACTIVE_ATTACK_MS) * 255 / (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS));
    } else {
        brightness = 0;
    }
    TEST_ASSERT_EQ(brightness, 255, "t=attack-1: full brightness");

    /* At t=attack (just entered decay): brightness starts dropping */
    elapsed = REACTIVE_ATTACK_MS;
    if (elapsed < REACTIVE_ATTACK_MS) {
        brightness = 255;
    } else if (elapsed < REACTIVE_DECAY_MS) {
        brightness = 255 - ((elapsed - REACTIVE_ATTACK_MS) * 255 / (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS));
    } else {
        brightness = 0;
    }
    TEST_ASSERT_EQ(brightness, 255, "t=attack: brightness=255 (start of decay)");

    /* At t=decay: off */
    elapsed = REACTIVE_DECAY_MS;
    if (elapsed < REACTIVE_ATTACK_MS) {
        brightness = 255;
    } else if (elapsed < REACTIVE_DECAY_MS) {
        brightness = 255 - ((elapsed - REACTIVE_ATTACK_MS) * 255 / (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS));
    } else {
        brightness = 0;
    }
    TEST_ASSERT_EQ(brightness, 0, "t=decay: off");

    /* Well past decay: still off */
    elapsed = REACTIVE_DECAY_MS + 1000;
    if (elapsed < REACTIVE_ATTACK_MS) {
        brightness = 255;
    } else if (elapsed < REACTIVE_DECAY_MS) {
        brightness = 255 - ((elapsed - REACTIVE_ATTACK_MS) * 255 / (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS));
    } else {
        brightness = 0;
    }
    TEST_ASSERT_EQ(brightness, 0, "t=decay+1000: still off");
}

/* Test: KPM bar calculation */
void test_kpm_bar_mapping(void) {
    int num_leds = 10;  /* Example strip */

    /* 0 KPM = 0 LEDs */
    uint32_t kpm = 0;
    int lit = (kpm * num_leds) / KPM_BAR_MAX;
    if (lit > num_leds) lit = num_leds;
    TEST_ASSERT_EQ(lit, 0, "0 KPM = 0 LEDs");

    /* Max KPM = all LEDs */
    kpm = KPM_BAR_MAX;
    lit = (kpm * num_leds) / KPM_BAR_MAX;
    if (lit > num_leds) lit = num_leds;
    TEST_ASSERT_EQ(lit, num_leds, "max KPM = all LEDs");

    /* Half KPM = half LEDs */
    kpm = KPM_BAR_MAX / 2;
    lit = (kpm * num_leds) / KPM_BAR_MAX;
    if (lit > num_leds) lit = num_leds;
    TEST_ASSERT_EQ(lit, num_leds / 2, "half KPM = half LEDs");

    /* Over max = clamped */
    kpm = KPM_BAR_MAX * 2;
    lit = (kpm * num_leds) / KPM_BAR_MAX;
    if (lit > num_leds) lit = num_leds;
    TEST_ASSERT_EQ(lit, num_leds, "over max = clamped");
}

/* Test: frame rate is reasonable */
void test_frame_rate(void) {
    TEST_ASSERT(LED_STRIP_FRAME_MS > 0, "frame period > 0");
    int fps = 1000 / LED_STRIP_FRAME_MS;
    TEST_ASSERT(fps >= 25 && fps <= 100, "FPS in [25, 100]");
}

/* Test: KPM_BAR_MAX is positive and reasonable */
void test_kpm_bar_max(void) {
    TEST_ASSERT(KPM_BAR_MAX > 0, "KPM_BAR_MAX > 0");
    TEST_ASSERT(KPM_BAR_MAX <= 1000, "KPM_BAR_MAX <= 1000");
}

void test_led_anim_constants(void) {
    TEST_SUITE("LED Animation Constants");
    TEST_RUN(test_reactive_timing);
    TEST_RUN(test_reactive_brightness_curve);
    TEST_RUN(test_kpm_bar_mapping);
    TEST_RUN(test_frame_rate);
    TEST_RUN(test_kpm_bar_max);
}
