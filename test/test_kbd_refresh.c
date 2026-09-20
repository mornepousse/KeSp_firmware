/* Bounded repetition of the last HID report over the radio link (pure logic).
 *
 * Measured at the bench on 2026-09-05: the relay kept re-emitting the last
 * report at 100 Hz continuously, keyboard idle, from the very first
 * keystroke of the board's life. This falsified the premise of §2.3 of the
 * Niphargus design ("emissions are event-driven, not periodic"), on which
 * bet R1 depends — a left half that emits permanently is permanently deaf,
 * and would never hear the right. */
#include "test_framework.h"
#include "../main/comm/rf/kbd_relay_tx.h"

static void test_repos_ne_reemet_pas(void)
{
    /* THE test of this file: a fresh state, never armed, must produce no
     * emission at all. This is what makes the premise of §2.3 true. */
    kbd_refresh_t r = {0};
    for (int i = 0; i < 100; i++)
        TEST_ASSERT(!kbd_refresh_step(&r), "at rest, no re-emission");
}

static void test_arme_puis_se_tait(void)
{
    kbd_refresh_t r = {0};
    kbd_refresh_arm(&r, 3);
    TEST_ASSERT(kbd_refresh_step(&r), "repeat 1");
    TEST_ASSERT(kbd_refresh_step(&r), "repeat 2");
    TEST_ASSERT(kbd_refresh_step(&r), "repeat 3");
    /* Then silence, for good — no permanent stream. */
    for (int i = 0; i < 50; i++)
        TEST_ASSERT(!kbd_refresh_step(&r), "silence after the last repeat");
}

static void test_rearmement_repart_du_plein(void)
{
    /* A keystroke during the repeat window must reset the counter to full,
     * not increment it nor let it run down. */
    kbd_refresh_t r = {0};
    kbd_refresh_arm(&r, 3);
    TEST_ASSERT(kbd_refresh_step(&r), "first repeat consumed");
    kbd_refresh_arm(&r, 3);
    TEST_ASSERT(kbd_refresh_step(&r), "re-armed: 1");
    TEST_ASSERT(kbd_refresh_step(&r), "re-armed: 2");
    TEST_ASSERT(kbd_refresh_step(&r), "re-armed: 3");
    TEST_ASSERT(!kbd_refresh_step(&r), "then silence");
}

static void test_zero_repetition_est_muet(void)
{
    /* Arming at zero disables self-repair without breaking the mechanism. */
    kbd_refresh_t r = {0};
    kbd_refresh_arm(&r, 0);
    TEST_ASSERT(!kbd_refresh_step(&r), "armed at zero -> mute");
}

static void test_cadence_du_relais(void)
{
    /* 100 ms at rest; 10 ms as soon as there's something to repeat, a held
     * key, a sync — AND as long as the left listens to the re-emitted right
     * (USB route): it's this polling that drains the nRF24's FIFO (3
     * frames), and at 100 ms the press and release of a right-side key
     * landed in the same round, leaving only the release (bench 2026-09-16:
     * "I'm losing a bunch of right-side keys" in USB). */
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, false, false, false), KBD_RELAY_REPOS_MS, "at rest");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(true,  false, false, false), KBD_RELAY_REFRESH_MS, "repair in progress");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, true,  false, false), KBD_RELAY_REFRESH_MS, "key held");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, false, true,  false), KBD_RELAY_REFRESH_MS, "sync");
    TEST_ASSERT_EQ(kbd_relay_cadence_ms(false, false, false, true),  KBD_RELAY_REFRESH_MS, "listening to the right's USB");
}

void test_kbd_refresh(void)
{
    printf("\n-- repetition bornee du rapport HID (radio) --\n");
    test_cadence_du_relais();
    test_repos_ne_reemet_pas();
    test_arme_puis_se_tait();
    test_rearmement_repart_du_plein();
    test_zero_repetition_est_muet();
}
