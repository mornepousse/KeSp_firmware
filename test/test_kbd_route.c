/* Tests for the wireless V2D HID output routing + VBUS debounce (pure logic). */
#include "test_framework.h"
#include "../main/comm/usb/usb_presence.h"
#include "../main/comm/rf/v2d_sleep.h"

static void test_route_usb_first(void)
{
    /* VBUS present always wins, regardless of relay state. */
    TEST_ASSERT_EQ(kbd_route_target(true,  true),  KBD_OUT_USB, "vbus+relay -> USB");
    TEST_ASSERT_EQ(kbd_route_target(true,  false), KBD_OUT_USB, "vbus only -> USB");
    /* No VBUS -> relay if paired. */
    TEST_ASSERT_EQ(kbd_route_target(false, true),  KBD_OUT_RF,  "no vbus + relay -> RF");
    /* Nothing reachable. */
    TEST_ASSERT_EQ(kbd_route_target(false, false), KBD_OUT_NONE, "no vbus, no relay -> NONE");
}

static void test_debounce_seeds_first_sample(void)
{
    vbus_debounce_t d = {0};
    TEST_ASSERT(vbus_debounce_step(&d, true, 1000, 50) == true, "first sample seeds true");
    vbus_debounce_t d2 = {0};
    TEST_ASSERT(vbus_debounce_step(&d2, false, 1000, 50) == false, "first sample seeds false");
}

static void test_debounce_holds_until_window(void)
{
    vbus_debounce_t d = {0};
    vbus_debounce_step(&d, true, 0, 50);              /* stable = true */
    /* raw drops to false @100; window counts from there → flips at >=150 */
    TEST_ASSERT(vbus_debounce_step(&d, false, 100, 50) == true,  "drop seen, window starts");
    TEST_ASSERT(vbus_debounce_step(&d, false, 149, 50) == true,  "49ms < window: still true");
    TEST_ASSERT(vbus_debounce_step(&d, false, 150, 50) == false, "50ms >= window: flips false");
}

static void test_debounce_rejects_bounce(void)
{
    vbus_debounce_t d = {0};
    vbus_debounce_step(&d, true, 0, 50);             /* stable = true */
    vbus_debounce_step(&d, false, 10, 50);           /* candidate false @10 */
    /* bounces back to true before the window elapses → timer restarts, stays true */
    TEST_ASSERT(vbus_debounce_step(&d, true, 20, 50) == true, "bounce back keeps true");
    TEST_ASSERT(vbus_debounce_step(&d, false, 30, 50) == true, "new false candidate @30");
    TEST_ASSERT(vbus_debounce_step(&d, false, 79, 50) == true, "79-30=49 < window: still true");
    TEST_ASSERT(vbus_debounce_step(&d, false, 80, 50) == false, "80-30=50 >= window: flips");
}

static void test_v2d_should_sleep(void)
{
    /* Sleep only on RF, only past the idle threshold. */
    TEST_ASSERT(!v2d_should_sleep(false, 999999, 60000), "USB path never sleeps");
    TEST_ASSERT(!v2d_should_sleep(true,  59999, 60000),  "RF but under threshold: no");
    TEST_ASSERT(v2d_should_sleep(true,  60000, 60000),   "RF at threshold: sleep");
    TEST_ASSERT(v2d_should_sleep(true,  120000, 60000),  "RF over threshold: sleep");
    TEST_ASSERT(!v2d_should_sleep(true, 0, 60000),       "RF just-active: no");
}

void test_kbd_route(void)
{
    printf("\n-- kbd_route / vbus_debounce / v2d_sleep --\n");
    test_route_usb_first();
    test_debounce_seeds_first_sample();
    test_debounce_holds_until_window();
    test_debounce_rejects_bounce();
    test_v2d_should_sleep();
}
