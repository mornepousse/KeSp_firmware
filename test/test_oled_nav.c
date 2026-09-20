#include "test_framework.h"
#include "oled_nav.h"

/* oled_nav_init does NOT arm the splash (wake/refresh case) → straight to HOME. */
static void test_nav_init_no_splash(void) {
    oled_nav_init(0);
    TEST_ASSERT_EQ(oled_nav_active(0),    OLED_SCR_HOME, "init alone → HOME (no splash on wake)");
    TEST_ASSERT_EQ(oled_nav_active(1000), OLED_SCR_HOME, "always HOME without BOOT");
}

/* Boot (OLED_EV_BOOT) → SPLASH for 2s → HOME. */
static void test_nav_boot_splash_then_home(void) {
    oled_nav_init(0);
    oled_nav_event(OLED_EV_BOOT, 0);
    TEST_ASSERT_EQ(oled_nav_active(0),    OLED_SCR_SPLASH, "BOOT → SPLASH");
    TEST_ASSERT_EQ(oled_nav_active(1999), OLED_SCR_SPLASH, "t=1999 → SPLASH");
    TEST_ASSERT_EQ(oled_nav_active(2000), OLED_SCR_HOME,   "t=2000 → HOME (splash done)");
    TEST_ASSERT_EQ(oled_nav_active(9000), OLED_SCR_HOME,   "idle = HOME");
}

/* Activity / layer change never change the screen (no more idle-tama). */
static void test_nav_activity_no_switch(void) {
    oled_nav_init(0);
    oled_nav_event(OLED_EV_ACTIVITY, 3000);
    TEST_ASSERT_EQ(oled_nav_active(3000), OLED_SCR_HOME, "activity → stays HOME");
    oled_nav_event(OLED_EV_LAYER_CHANGED, 5000);
    TEST_ASSERT_EQ(oled_nav_active(50000), OLED_SCR_HOME, "long idle → always HOME (no more TAMA)");
}

/* Keycode cycle: HOME → STATS → HOME. */
static void test_nav_dispkey_cycle(void) {
    oled_nav_init(0);
    TEST_ASSERT_EQ(oled_nav_active(3000), OLED_SCR_HOME, "idle HOME");
    oled_nav_event(OLED_EV_DISP_KEY, 3000);
    TEST_ASSERT_EQ(oled_nav_active(3000), OLED_SCR_STATS, "cycle → STATS");
    oled_nav_event(OLED_EV_DISP_KEY, 3001);
    TEST_ASSERT_EQ(oled_nav_active(3001), OLED_SCR_HOME,  "cycle → HOME");
}

/* The keycode cuts an ongoing boot splash. */
static void test_nav_dispkey_cuts_splash(void) {
    oled_nav_init(0);
    oled_nav_event(OLED_EV_BOOT, 0);
    TEST_ASSERT_EQ(oled_nav_active(100), OLED_SCR_SPLASH, "splash active");
    oled_nav_event(OLED_EV_DISP_KEY, 100);
    TEST_ASSERT_EQ(oled_nav_active(100), OLED_SCR_STATS, "keycode cuts the splash → STATS");
}

void test_oled_nav(void) {
    TEST_SUITE("OLED nav — pure state machine");
    TEST_RUN(test_nav_init_no_splash);
    TEST_RUN(test_nav_boot_splash_then_home);
    TEST_RUN(test_nav_activity_no_switch);
    TEST_RUN(test_nav_dispkey_cycle);
    TEST_RUN(test_nav_dispkey_cuts_splash);
}
