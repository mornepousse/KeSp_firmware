/* 5 V handshake for the inter-half link.
 *
 * Hardware stake: closing the load switch before recognizing the half on the
 * other side means putting 5 V on an exposed connector — exactly the
 * historical killer of splits that this design is trying to eliminate. The
 * rule tested here is: LINK_5V_EN NEVER comes up without a successful
 * handshake, and it drops as soon as the half on the other side goes quiet.
 */
#include <string.h>
#include "test_framework.h"
#include "../main/comm/link/link_handshake.h"

static void test_starts_dead(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "idle at start");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead at start");
}

static void test_probe_then_ack_enables_5v(void)
{
    link_hs_t hs;
    link_hs_init(&hs);

    /* USB present: we probe. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 1000);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_SEND_PROBE, "USB present -> probe");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V still dead during probe");

    /* The half on the other side answers. */
    a = link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 1050);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ENABLE_5V, "peer recognized -> close the switch");
    TEST_ASSERT(link_hs_5v_enabled(&hs), "5V alive after handshake");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "link established");
}

static void test_never_enables_without_ack(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);

    /* Noise on the line, not an ACK: nothing should come up. */
    for (uint32_t t = 1; t < LINK_HS_PROBE_TIMEOUT_MS; t += 10) {
        link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, t);
        TEST_ASSERT(a != LINK_HS_ACT_ENABLE_5V, "no 5V without a handshake");
        TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V stays dead");
    }
}

static void test_probe_timeout_returns_to_idle(void)
{
    link_hs_t hs;
    link_hs_init(&hs);

    /* Test precondition: we must actually be in PROBING before the
     * timeout, otherwise a stub that ignored USB_PRESENT would pass this
     * test for the wrong reasons (NONE/IDLE/5V-dead is also its output). */
    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_SEND_PROBE, "USB present -> probe");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "probing before the timeout");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PROBE_TIMEOUT_MS);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "no reply -> give up");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "back to idle");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead after giving up");
}

static void test_peer_ack_ignored_in_idle(void)
{
    /* An unsolicited ACK, before any probe: we never asked for it, we don't
     * believe it. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 5);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "unsolicited ACK in IDLE -> nothing");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "stays idle");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead");
}

static void test_peer_frame_ignored_in_probing(void)
{
    /* A frame received before the ACK must not cause a transition: only
     * PEER_ACK counts as recognition of the half on the other side. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PEER_FRAME, 10);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "frame without ACK in PROBING -> nothing");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "stays probing");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead");
}

static void test_usb_gone_already_dead_from_idle(void)
{
    /* USB_GONE while 5V is already dead (never came up): no redundant
     * DISABLE_5V, but no crash or inconsistent state either. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_GONE, 5);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "5V already dead in IDLE -> no DISABLE_5V");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "stays idle");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead");
}

static void test_usb_gone_already_dead_from_probing(void)
{
    /* Same thing from PROBING: the probe went out but 5V is not
     * yet up, so USB_GONE must not claim to have cut it.
     *
     * Precondition checked explicitly: without it, this test would pass
     * identically against a stub where USB_PRESENT did nothing (the machine
     * would stay in IDLE) — the final state (IDLE, 5V dead) would be the same
     * without ever going through PROBING, and the test's name would lie. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_SEND_PROBE, "USB present -> probe");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "probing before USB_GONE");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_GONE, 10);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "5V already dead in PROBING -> no DISABLE_5V");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "back to idle");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead");
}

static void test_late_ack_after_timeout_still_accepted(void)
{
    /* Pinned behavior, not a bug: if no TICK made PROBING fall back before the
     * ACK arrives — even past the nominal delay — the ACK is still required
     * and accepted. Accepting a late ACK beats forcing a re-probe. Do not
     * change this behavior without an explicit decision; this test pins it.
     */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PEER_ACK,
                                       LINK_HS_PROBE_TIMEOUT_MS + 50);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ENABLE_5V, "late ACK still accepted (pinned)");
    TEST_ASSERT(link_hs_5v_enabled(&hs), "5V up despite the delay");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "link established despite the delay");
}

static void test_unknown_state_falls_safe_without_init(void)
{
    /* Call contract violated: a stack struct never passed through
     * link_hs_init(), filled with arbitrary memory (here 0xFF everywhere). The
     * switch's `default:` must fall back to the safe side — 5V dead — rather
     * than preserving a garbage en_5v that would read `true` even though no
     * ACT_ENABLE_5V was ever emitted. */
    link_hs_t hs;
    memset(&hs, 0xFF, sizeof(hs));
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, 0);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "unknown state -> no action");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "unknown state -> 5V falls back dead");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "unknown state -> falls back to IDLE");
}

static void test_peer_silence_drops_5v(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 10);
    TEST_ASSERT(link_hs_5v_enabled(&hs), "5V alive");

    /* The peer goes quiet: cable yanked out. The switch must reopen. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, 10 + LINK_HS_PEER_TIMEOUT_MS);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_DISABLE_5V, "peer silence -> reopen");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead after the yank");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "back to idle");
}

static void test_peer_traffic_keeps_link_up(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 10);

    /* Regular traffic: the link holds indefinitely. */
    for (uint32_t t = 10; t < 10 * LINK_HS_PEER_TIMEOUT_MS; t += LINK_HS_PEER_TIMEOUT_MS / 2) {
        link_hs_step(&hs, LINK_HS_EV_PEER_FRAME, t);
        TEST_ASSERT(link_hs_5v_enabled(&hs), "traffic keeps the link up");
    }
}

static void test_usb_lost_drops_5v(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&hs, LINK_HS_EV_PEER_ACK, 10);

    /* Unplugged: nobody is being fed anymore. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_USB_GONE, 20);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_DISABLE_5V, "USB gone -> reopen");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead without USB");
}

static void test_no_probe_without_usb(void)
{
    /* On battery alone, we don't probe: we have nothing to give. Even after a
     * long wait — several multiples of the re-probe interval — the absence of
     * USB must remain the only reason blocking everything. */
    link_hs_t hs;
    link_hs_init(&hs);
    for (uint32_t t = 0; t <= 5 * LINK_HS_REPROBE_INTERVAL_MS; t += 100) {
        link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, t);
        TEST_ASSERT_EQ(a, LINK_HS_ACT_NONE, "no USB -> never probe, even after a long while");
        TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "stays idle");
    }
}

static void test_lost_probe_eventually_reprobes(void)
{
    /* A lost ACK must not condemn the link until the cable is replugged:
     * USB_PRESENT is an edge event, it won't come back on its own. IDLE must
     * re-probe as long as h->usb stays true. */
    link_hs_t hs;
    link_hs_init(&hs);

    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_SEND_PROBE, "USB present -> first probe");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "probing");

    /* The probe gets lost: timeout, back to IDLE without ever raising 5V. */
    link_hs_action_t a1 = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PROBE_TIMEOUT_MS);
    TEST_ASSERT_EQ(a1, LINK_HS_ACT_NONE, "probe timeout -> give up");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "back to idle");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V still dead");

    /* Not yet time to re-probe: the interval hasn't elapsed. */
    link_hs_action_t a2 = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PROBE_TIMEOUT_MS + 1);
    TEST_ASSERT_EQ(a2, LINK_HS_ACT_NONE, "not yet time to re-probe");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "still idle");

    /* The re-probe interval has elapsed since entering IDLE: the machine
     * restarts on its own, without a new USB_PRESENT event. */
    uint32_t t_reprobe = LINK_HS_PROBE_TIMEOUT_MS + LINK_HS_REPROBE_INTERVAL_MS;
    link_hs_action_t a3 = link_hs_step(&hs, LINK_HS_EV_TICK, t_reprobe);
    TEST_ASSERT_EQ(a3, LINK_HS_ACT_SEND_PROBE, "lost probe -> automatic re-probe after the interval");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "back to probing");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "probing is not raising 5V");
}

static void test_probe_timeout_wraps_uint32_cleanly(void)
{
    /* link_hs_step handles the uint32 overflow of now_ms via unsigned
     * subtraction. We had no test forcing this path — a future "fix" like
     * `if (now_ms < since_ms) return;` would silently break the property.
     * since_ms close to UINT32_MAX, now_ms after wrapping to zero.
     */
    link_hs_t hs;
    link_hs_init(&hs);
    hs.usb = true;
    hs.state = LINK_HS_PROBING;
    hs.since_ms = 0xFFFFFFF0u; /* entering PROBING right before the overflow */

    /* now_ms has wrapped, but the real elapsed time is only 199 ms:
     * 0xFFFFFFF0 + 199 wraps to 183. Not yet the timeout. */
    uint32_t elapsed_199 = (uint32_t)(0xFFFFFFF0u + (LINK_HS_PROBE_TIMEOUT_MS - 1));
    link_hs_action_t a0 = link_hs_step(&hs, LINK_HS_EV_TICK, elapsed_199);
    TEST_ASSERT_EQ(a0, LINK_HS_ACT_NONE, "199 ms after the wrap -> not yet the timeout");
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "still probing");

    /* Exactly 200 ms later (still after the wrap): the timeout must trigger
     * normally, despite now_ms < since_ms in signed arithmetic.
     */
    hs.since_ms = 0xFFFFFFF0u;
    hs.state = LINK_HS_PROBING;
    uint32_t elapsed_200 = (uint32_t)(0xFFFFFFF0u + LINK_HS_PROBE_TIMEOUT_MS);
    link_hs_action_t a1 = link_hs_step(&hs, LINK_HS_EV_TICK, elapsed_200);
    TEST_ASSERT_EQ(a1, LINK_HS_ACT_NONE, "probe timeout triggered despite the now_ms wrap");
    TEST_ASSERT_EQ(hs.state, LINK_HS_IDLE, "back to idle despite the wrap");
}

/* ── I7: the RECEIVER side of the handshake ──────────────────────────────────
 *
 * The hardware contract requires BOTH halves to close their switch for
 * current to flow (NIPHARGUS_V2_HARDWARE.md). The machine only modeled the
 * transmitter: the half on battery never closed its own, so a successful
 * handshake let no current through at all.
 *
 * The safety invariant becomes: en_5v only goes true after a VERIFIED
 * EXCHANGE with the peer — either we probed and got an ACK, or we were
 * probed and answered. A single closed switch still lets nothing through.
 */
static void test_probed_answers_and_closes_its_own_switch(void)
{
    link_hs_t hs;
    link_hs_init(&hs);

    /* On battery, without USB: it's the peer that probes. */
    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PROBED, 100);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ACK_AND_ENABLE_5V,
                   "probed -> it answers AND closes its own switch");
    TEST_ASSERT(link_hs_5v_enabled(&hs), "its side is closed, current can flow");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "the peer is recognized as alive");
}

static void test_probed_while_probing_still_pairs(void)
{
    /* Both halves plugged in probe at the same time: each receives the other's
     * probe while it's waiting for an ACK. The link must still establish
     * rather than getting stuck in mutual waiting. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_USB_PRESENT, 0);
    TEST_ASSERT_EQ(hs.state, LINK_HS_PROBING, "waiting for an ACK");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_PROBED, 10);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_ACK_AND_ENABLE_5V, "crossed probes -> we answer and close");
    TEST_ASSERT_EQ(hs.state, LINK_HS_UP, "the link establishes despite the crossing");
}

static void test_probed_side_drops_5v_when_peer_goes_silent(void)
{
    /* The receiver has no USB: it will never get a USB_GONE. Its only way to
     * know the cable is gone is the peer's silence. */
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_PROBED, 0);
    TEST_ASSERT(link_hs_5v_enabled(&hs), "closed after the probe");

    link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, LINK_HS_PEER_TIMEOUT_MS);
    TEST_ASSERT_EQ(a, LINK_HS_ACT_DISABLE_5V, "peer silence -> we reopen");
    TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V dead after the jack is yanked out");
}

static void test_repeated_probes_keep_the_link_alive(void)
{
    link_hs_t hs;
    link_hs_init(&hs);
    link_hs_step(&hs, LINK_HS_EV_PROBED, 0);
    /* The peer re-probes periodically: each probe counts as a sign of life. */
    for (uint32_t t = 0; t < 5 * LINK_HS_PEER_TIMEOUT_MS; t += LINK_HS_PEER_TIMEOUT_MS / 2) {
        link_hs_step(&hs, LINK_HS_EV_PROBED, t);
        TEST_ASSERT(link_hs_5v_enabled(&hs), "repeated probes keep the link alive");
    }
}

static void test_tick_alone_never_closes_the_switch(void)
{
    /* The invariant, restated: without the slightest exchange with the peer —
     * neither an ACK received nor a probe received — nothing must close the
     * switch, no matter how much time has elapsed. */
    link_hs_t hs;
    link_hs_init(&hs);
    hs.usb = true;   /* even with current to give */
    for (uint32_t t = 0; t < 10 * LINK_HS_REPROBE_INTERVAL_MS; t += 37) {
        link_hs_action_t a = link_hs_step(&hs, LINK_HS_EV_TICK, t);
        TEST_ASSERT(a != LINK_HS_ACT_ENABLE_5V && a != LINK_HS_ACT_ACK_AND_ENABLE_5V,
                    "no tick closes the switch without an exchange with the peer");
        TEST_ASSERT(!link_hs_5v_enabled(&hs), "5V stays dead");
    }
}

/* ── Keeping the link alive in UP ─────────────────────────────────────────────
 *
 * Observed at the bench on 2026-09-11, first 5V closing: the link came up,
 * then dropped 500 ms later. In UP, the transmitter no longer probed, so
 * the receiver had nothing left to acknowledge, and LINK_HS_PEER_TIMEOUT_MS
 * expired. The design relied on MATRIX frames over the wired path to keep
 * the link alive — a path that doesn't exist, the halves talk over radio.
 *
 * Same pattern as three September outages: "emit on change" and
 * "release on silence" don't compose. Same remedy: as long as we have
 * current to give, we say so periodically. */

static void test_up_avec_usb_resonde_periodiquement(void)
{
    link_hs_t h; link_hs_init(&h);
    link_hs_step(&h, LINK_HS_EV_USB_PRESENT, 1000);          /* -> PROBING */
    link_hs_step(&h, LINK_HS_EV_PEER_ACK, 1010);             /* -> UP */
    TEST_ASSERT(h.state == LINK_HS_UP && h.en_5v, "link comes up");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1100) == LINK_HS_ACT_NONE,
                "not yet: the period hasn't elapsed");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1010 + LINK_HS_KEEPALIVE_MS)
                == LINK_HS_ACT_SEND_PROBE, "period elapsed: we re-probe");
    TEST_ASSERT(h.state == LINK_HS_UP && h.en_5v, "and we STAY in UP, 5V closed");
}

static void test_ack_en_up_entretient_le_lien(void)
{
    /* THE bench-side test of this file: probe + ACK every period must hold
     * the link indefinitely, well past PEER_TIMEOUT. */
    link_hs_t h; link_hs_init(&h);
    link_hs_step(&h, LINK_HS_EV_USB_PRESENT, 0);
    link_hs_step(&h, LINK_HS_EV_PEER_ACK, 10);
    uint32_t t = 10;
    for (int i = 0; i < 20; i++) {
        t += LINK_HS_KEEPALIVE_MS;
        TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, t) == LINK_HS_ACT_SEND_PROBE, "re-probes");
        TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_PEER_ACK, t + 5) == LINK_HS_ACT_NONE, "ACK received");
        TEST_ASSERT(h.state == LINK_HS_UP && h.en_5v, "still UP after several seconds");
    }
}

static void test_up_sans_usb_ne_sonde_pas(void)
{
    /* The receiver (no USB) came up because it was probed; it never probes
     * itself — it has no current to give. */
    link_hs_t h; link_hs_init(&h);
    link_hs_step(&h, LINK_HS_EV_PROBED, 1000);               /* -> UP, by answering */
    TEST_ASSERT(h.state == LINK_HS_UP && !h.usb, "receiver up, without USB");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1000 + LINK_HS_KEEPALIVE_MS)
                == LINK_HS_ACT_NONE, "it doesn't probe");
    TEST_ASSERT(link_hs_step(&h, LINK_HS_EV_TICK, 1000 + LINK_HS_PEER_TIMEOUT_MS)
                == LINK_HS_ACT_DISABLE_5V, "and it expires if nobody probes it anymore");
}

static void test_la_periode_tient_sous_le_timeout(void)
{
    TEST_ASSERT(LINK_HS_KEEPALIVE_MS * 2 < LINK_HS_PEER_TIMEOUT_MS,
                "two keepalives fit within the peer's expiry delay");
}

void test_link_handshake(void)
{
    test_up_avec_usb_resonde_periodiquement();
    test_ack_en_up_entretient_le_lien();
    test_up_sans_usb_ne_sonde_pas();
    test_la_periode_tient_sous_le_timeout();
    printf("\n-- poignée de main 5 V du lien --\n");
    test_starts_dead();
    test_probed_answers_and_closes_its_own_switch();
    test_probed_while_probing_still_pairs();
    test_probed_side_drops_5v_when_peer_goes_silent();
    test_repeated_probes_keep_the_link_alive();
    test_tick_alone_never_closes_the_switch();
    test_probe_then_ack_enables_5v();
    test_never_enables_without_ack();
    test_probe_timeout_returns_to_idle();
    test_peer_silence_drops_5v();
    test_peer_traffic_keeps_link_up();
    test_usb_lost_drops_5v();
    test_no_probe_without_usb();
    test_peer_ack_ignored_in_idle();
    test_peer_frame_ignored_in_probing();
    test_usb_gone_already_dead_from_idle();
    test_usb_gone_already_dead_from_probing();
    test_late_ack_after_timeout_still_accepted();
    test_unknown_state_falls_safe_without_init();
    test_lost_probe_eventually_reprobes();
    test_probe_timeout_wraps_uint32_cleanly();
}
