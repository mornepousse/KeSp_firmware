/* The dongle's two RF slots: presence supervision and safe fallback.
 *
 * The dongle no longer has two keyboard halves in front of it. It has a keyboard
 * (the Niphargus master half, which sends it already-finished HID) and a mouse
 * (Conchodytes). These two have nothing to do with each other, and that's
 * exactly what these tests lock down: **losing the mouse must not
 * release the keys**. Under the old reading — two halves of a single
 * keyboard — releasing everything on the loss of either one was correct. It no
 * longer is, and nothing in the code recalls that except here.
 *
 * The fallback exists because a link that goes silent freezes the last report received
 * on the host: if the last thing received was "Super pressed", it stays pressed
 * until reconnection.
 */
#include "test_framework.h"
#include "../main/comm/rf/rf_slot.h"

/* ── The core: one slot does not drag the other along ────────────────────── */

static void test_losing_the_mouse_never_releases_keys(void)
{
    rf_slot_link_t mouse = {0};
    rf_slot_link_rx(&mouse, 1000);
    TEST_ASSERT_EQ(rf_slot_link_check(&mouse, RF_SLOT_MOUSE, 4000, 2500),
                   RF_SAFE_RELEASE_BUTTONS,
                   "silent mouse → release its buttons");

    rf_slot_link_t kbd = {0};
    rf_slot_link_rx(&kbd, 1000);
    TEST_ASSERT_EQ(rf_slot_link_check(&kbd, RF_SLOT_KBD, 4000, 2500),
                   RF_SAFE_RELEASE_KEYS,
                   "silent keyboard → release its keys");
}

/* ── Presence ────────────────────────────────────────────────────── */

static void test_a_slot_never_seen_has_nothing_to_release(void)
{
    /* Before the first packet, there is nothing to release: sending an empty
     * report at startup would overwrite what another path just sent. */
    rf_slot_link_t l = {0};
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 999999, 2500), RF_SAFE_NONE,
                   "never seen → nothing to release");
}

static void test_a_fresh_link_is_left_alone(void)
{
    rf_slot_link_t l = {0};
    rf_slot_link_rx(&l, 1000);
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 1000, 2500), RF_SAFE_NONE, "right now");
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 3499, 2500), RF_SAFE_NONE,
                   "just before the deadline");
    TEST_ASSERT(l.up, "and the link is still declared present");
}

static void test_the_deadline_is_inclusive(void)
{
    rf_slot_link_t l = {0};
    rf_slot_link_rx(&l, 1000);
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 3500, 2500), RF_SAFE_RELEASE_KEYS,
                   "exactly at the deadline, the link is lost");
    TEST_ASSERT(!l.up, "and marked absent");
}

/* ── Only once ──────────────────────────────────────────────── */

static void test_the_release_fires_once_not_every_tick(void)
{
    /* The RF loop calls this every 10 ms. If the loss re-triggered
     * on every turn, a cut link would flood the HID endpoint with empty reports. */
    rf_slot_link_t l = {0};
    rf_slot_link_rx(&l, 1000);
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 4000, 2500), RF_SAFE_RELEASE_KEYS,
                   "first time the loss is noticed");
    for (uint32_t t = 4010; t < 4500; t += 10)
        TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, t, 2500), RF_SAFE_NONE,
                       "then nothing more as long as the link doesn't come back");
}

static void test_a_returning_link_can_be_lost_again(void)
{
    rf_slot_link_t l = {0};
    rf_slot_link_rx(&l, 1000);
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 4000, 2500), RF_SAFE_RELEASE_KEYS, "lost");
    rf_slot_link_rx(&l, 5000);
    TEST_ASSERT(l.up, "a received packet brings the link back up");
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 6000, 2500), RF_SAFE_NONE, "fresh again");
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 8000, 2500), RF_SAFE_RELEASE_KEYS,
                   "and losable again");
}

/* ── The millisecond counter wraps around ────────────────────────── */

static void test_survives_the_millisecond_counter_wrapping(void)
{
    /* esp_timer_get_time()/1000 truncated to uint32 wraps around after ~49
     * days. A permanently plugged-in dongle gets there. An unsigned
     * subtraction survives the wraparound; a `now > last + timeout`
     * comparison would not. */
    rf_slot_link_t l = {0};
    rf_slot_link_rx(&l, 0xFFFFFF00u);
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 0x00000300u, 2500), RF_SAFE_NONE,
                   "1 s later, straddling the wraparound: still fresh");
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_KBD, 0x00001000u, 2500), RF_SAFE_RELEASE_KEYS,
                   "and the loss is seen at the right time, not 49 days later");
}

/* ── Arguments ───────────────────────────────────────────────────── */

static void test_null_is_inert(void)
{
    rf_slot_link_rx(NULL, 1000);   /* must not crash */
    TEST_ASSERT_EQ(rf_slot_link_check(NULL, RF_SLOT_KBD, 9999, 2500), RF_SAFE_NONE, "NULL → nothing");
}

static void test_the_two_slots_are_distinct(void)
{
    TEST_ASSERT(RF_SLOT_KBD != RF_SLOT_MOUSE, "keyboard and mouse are not the same slot");
    TEST_ASSERT_EQ(RF_SLOT_COUNT, 2, "the dongle has two, no more");
}

void test_rf_slot(void)
{
    printf("\n-- slots RF du dongle --\n");
    test_losing_the_mouse_never_releases_keys();
    test_a_slot_never_seen_has_nothing_to_release();
    test_a_fresh_link_is_left_alone();
    test_the_deadline_is_inclusive();
    test_the_release_fires_once_not_every_tick();
    test_a_returning_link_can_be_lost_again();
    test_survives_the_millisecond_counter_wrapping();
    test_null_is_inert();
    test_the_two_slots_are_distinct();
}
