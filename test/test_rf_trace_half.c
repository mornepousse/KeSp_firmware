/* rf-scope, half side: the pure decisions — OBSERVE_TX decode and PLOS
 * delta, TX result, the session
 * control (periodic cadence, sleeping/woke records), the UART0 KS
 * assembler. The request payload, the auto-off timer and the frame identity
 * are Task 2's (rf_trace_ctl.c, test_rf_trace_ctl.c), reused as is. Spec: docs/superpowers/specs/2026-10-20-rf-scope-design.md. */
#include "test_framework.h"
#include "../main/comm/rf/rf_trace_half.h"
#include "../main/comm/cdc/cdc_binary_protocol.h"
#include <string.h>

/* ── TX result and OBSERVE_TX ── */

static void test_tx_result(void)
{
    TEST_ASSERT_EQ(rf_trace_tx_result(true, 0x2E), RF_TRACE_TX_ACK, "TX_DS");
    TEST_ASSERT_EQ(rf_trace_tx_result(false, 0x1E), RF_TRACE_TX_MAX_RT, "MAX_RT");
    TEST_ASSERT_EQ(rf_trace_tx_result(false, 0x0E), RF_TRACE_TX_TIMEOUT, "neither flag: timeout");
    TEST_ASSERT_EQ(rf_trace_tx_result(true, 0x00), RF_TRACE_TX_ACK, "the driver's verdict wins");
}

static void test_observe_arc_and_plos_delta(void)
{
    rf_trace_plos_t st = { 0, 0 }; uint8_t arc = 0xFF;
    /* First record of session 7: PLOS = 3 from before the session: a baseline. */
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x32, 7, RF_PLOS_NO_RESET, &arc), 0, "new session: baseline 0");
    TEST_ASSERT_EQ(arc, 2, "ARC_CNT = low nibble");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x3F, 7, RF_PLOS_NO_RESET, &arc), 0, "no loss since");
    TEST_ASSERT_EQ(arc, 15, "15 retries");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x4F, 7, RF_PLOS_NO_RESET, &arc), 1, "one MAX_RT: +1");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x10, 7, RF_PLOS_NO_RESET, &arc), 1,
                   "went down: an unreported RF_CH write, count from zero");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x10, 7, RF_PLOS_RESET_SINCE, &arc), 1,
                   "RF_CH written since: the whole counter is new");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x10, 7, RF_PLOS_NO_RESET, &arc), 0, "unchanged");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x11, 8, RF_PLOS_RESET_SINCE, &arc), 0,
                   "new session wins over RESET_SINCE (untraced sends may have counted)");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x11, 8, RF_PLOS_RESET_NOW, &arc), 1,
                   "excursion: RF_CH written just before this packet, the counter is this packet's");
    st.session = 0;
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0x01, 9, RF_PLOS_RESET_NOW, &arc), 0,
                   "excursion in a new session: still exact (0 lost)");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0xF0, 9, RF_PLOS_NO_RESET, &arc), 15, "0 -> 15");
    TEST_ASSERT_EQ(rf_trace_observe_step(&st, 0xF0, 9, RF_PLOS_NO_RESET, &arc), 0, "saturated: 0, result says MAX_RT");
}

/* ── Session control ── */

static void test_ctl_off_does_nothing(void)
{
    rf_trace_sess_t c; uint32_t w = 0;
    rf_trace_sess_init(&c);
    TEST_ASSERT_EQ(c.session, 0, "off at boot");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 5000, &w), RF_TRACE_ACT_NONE, "off: nothing due");
    TEST_ASSERT(w == UINT32_MAX, "off: block forever (no poll)");
    TEST_ASSERT(!rf_trace_sess_sleep(&c), "off: no sleeping record");
    TEST_ASSERT(!rf_trace_sess_wake(&c, 6000), "off: no woke record");
}

static void test_ctl_periodic_then_auto_off(void)
{
    rf_trace_sess_t c; uint32_t w = 0;
    rf_trace_sess_init(&c);
    rf_trace_sess_start(&c, 10000, 3);
    TEST_ASSERT(c.session != 0, "on");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 10000, &w), RF_TRACE_ACT_PERIODIC, "first periodic at once");
    TEST_ASSERT_EQ(w, RF_TRACE_PERIODIC_MS, "next in one period");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 10400, &w), RF_TRACE_ACT_NONE, "nothing due early (a notify wake)");
    TEST_ASSERT_EQ(w, 600, "remaining wait");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 11000, &w), RF_TRACE_ACT_PERIODIC, "1 s");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 12050, &w), RF_TRACE_ACT_PERIODIC, "2 s (late wake)");
    TEST_ASSERT_EQ(w, 950, "cadence kept on the grid, not drifting");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 12999, &w), RF_TRACE_ACT_NONE, "1 ms before the end");
    TEST_ASSERT_EQ(w, 1, "wakes exactly at the deadline");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 13000, &w), RF_TRACE_ACT_EXPIRED, "3 s: auto-off");
    TEST_ASSERT_EQ(c.session, 0, "off");
    TEST_ASSERT(w == UINT32_MAX, "back to blocking forever");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 14000, &w), RF_TRACE_ACT_NONE, "expired once only");
}

static void test_ctl_restart_takes_a_new_session(void)
{
    rf_trace_sess_t c; uint32_t w;
    rf_trace_sess_init(&c);
    rf_trace_sess_start(&c, 0, 10);
    uint32_t s1 = c.session;
    rf_trace_sess_stop(&c);
    TEST_ASSERT_EQ(c.session, 0, "stopped");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 100, &w), RF_TRACE_ACT_NONE, "stop: no auto-off record either");
    rf_trace_sess_start(&c, 200, 0);
    TEST_ASSERT(c.session != 0 && c.session != s1, "new non-zero session");
    TEST_ASSERT_EQ(c.timer.dur_ms, RF_TRACE_DEFAULT_S * 1000u, "0 s: default");
    c.last_session = 0xFFFFFFFFu;
    rf_trace_sess_start(&c, 300, 5);
    TEST_ASSERT(c.session != 0, "numbering skips 0 on wrap");
}

static void test_ctl_sleep_and_wake_records(void)
{
    rf_trace_sess_t c; uint32_t w;
    rf_trace_sess_init(&c);
    rf_trace_sess_start(&c, 0, 600);
    rf_trace_sess_tick(&c, 0, &w);
    TEST_ASSERT(rf_trace_sess_sleep(&c), "going to sleep: one SLEEPING record");
    TEST_ASSERT(!rf_trace_sess_sleep(&c), "deep sleep re-calls the hooks: not twice");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 5000, &w), RF_TRACE_ACT_NONE, "asleep: no periodic record");
    TEST_ASSERT(rf_trace_sess_wake(&c, 120000), "awake: one WOKE record");
    TEST_ASSERT(!rf_trace_sess_wake(&c, 120001), "not twice");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 120500, &w), RF_TRACE_ACT_NONE, "WOKE stands for this period");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 121000, &w), RF_TRACE_ACT_PERIODIC, "then the cadence resumes");
}

static void test_ctl_periodic_stays_on_its_grid(void)
{
    /* Far from the deadline: the 1 s grid alone sets the wait. A late wake
     * (busy CPU) must not push every following record later. */
    rf_trace_sess_t c; uint32_t w;
    rf_trace_sess_init(&c);
    rf_trace_sess_start(&c, 0, 600);
    rf_trace_sess_tick(&c, 0, &w);
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 1050, &w), RF_TRACE_ACT_PERIODIC, "late by 50 ms");
    TEST_ASSERT_EQ(w, 950, "next one still at 2000, not 2050");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 5300, &w), RF_TRACE_ACT_PERIODIC, "a long gap: one record, not a burst");
    TEST_ASSERT_EQ(w, 1000, "restarts from now after a gap");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 5301, &w), RF_TRACE_ACT_NONE, "no catch-up records");
}

static void test_ctl_auto_off_across_the_wrap(void)
{
    rf_trace_sess_t c; uint32_t w;
    uint32_t t0 = 0xFFFFFFFFu - 1999u;   /* started 2 s before the 32-bit ms wrap */
    rf_trace_sess_init(&c);
    rf_trace_sess_start(&c, t0, 10);
    rf_trace_sess_tick(&c, t0, &w);
    TEST_ASSERT(rf_trace_sess_tick(&c, t0 + 5000u, &w) != RF_TRACE_ACT_EXPIRED, "5 s later, across the wrap: on");
    TEST_ASSERT(c.session != 0, "still on");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, t0 + 10000u, &w), RF_TRACE_ACT_EXPIRED, "10 s later: off");
}

static void test_ctl_expiry_during_sleep(void)
{
    rf_trace_sess_t c; uint32_t w;
    rf_trace_sess_init(&c);
    rf_trace_sess_start(&c, 0, 60);
    TEST_ASSERT(rf_trace_sess_sleep(&c), "sleeping");
    TEST_ASSERT(!rf_trace_sess_wake(&c, 3600000), "woke an hour later: no WOKE record");
    TEST_ASSERT_EQ(c.session, 0, "the duration ran out during the sleep: off");
    TEST_ASSERT_EQ(rf_trace_sess_tick(&c, 3600001, &w), RF_TRACE_ACT_NONE, "nothing after");
}

/* ── KS assembler and KR encoder ── */

static size_t ks_frame(uint8_t *out, uint8_t cmd, const uint8_t *p, uint16_t n)
{
    out[0] = 'K'; out[1] = 'S'; out[2] = cmd; out[3] = (uint8_t)n; out[4] = (uint8_t)(n >> 8);
    if (n) memcpy(out + 5, p, n);
    out[5 + n] = ks_crc8(p, n);
    return 6u + n;
}

static int feed_all(rf_trace_ks_rx_t *rx, const uint8_t *b, size_t n, int *at)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++)
        if (rf_trace_ks_rx_feed(rx, b[i])) { frames++; if (at) *at = (int)i; }
    return frames;
}

static void test_ks_rx_assembles_a_request(void)
{
    rf_trace_ks_rx_t rx; uint8_t f[32]; int at = -1;
    const uint8_t p[] = { RF_TRACE_SUB_START, 0x2C, 0x01 };
    size_t n = ks_frame(f, KS_CMD_RF_TRACE, p, sizeof p);
    rf_trace_ks_rx_reset(&rx);
    TEST_ASSERT_EQ(feed_all(&rx, f, n, &at), 1, "one frame");
    TEST_ASSERT_EQ(at, (int)n - 1, "complete on the CRC byte");
    TEST_ASSERT_EQ(rx.cmd, KS_CMD_RF_TRACE, "cmd");
    TEST_ASSERT(rx.len == 3 && memcmp(rx.payload, p, 3) == 0, "payload");

    const uint8_t stop[] = { RF_TRACE_SUB_STOP };
    n = ks_frame(f, KS_CMD_RF_TRACE, stop, 1);
    TEST_ASSERT_EQ(feed_all(&rx, f, n, NULL), 1, "the next one too");
    TEST_ASSERT(rx.len == 1 && rx.payload[0] == RF_TRACE_SUB_STOP, "stop payload");

    n = ks_frame(f, 0x04, NULL, 0);
    TEST_ASSERT_EQ(feed_all(&rx, f, n, NULL), 1, "empty payload");
    TEST_ASSERT(rx.cmd == 0x04 && rx.len == 0, "any command is assembled");
}

static void test_ks_rx_resyncs(void)
{
    rf_trace_ks_rx_t rx; uint8_t buf[96]; size_t n = 0;
    const uint8_t p[] = { RF_TRACE_SUB_START, 0x05, 0x00 };
    rf_trace_ks_rx_reset(&rx);

    /* Noise, a lone K, a KK, a KR (a reply echoed back), then the frame. */
    const uint8_t noise[] = { 0x00, 0xFF, 'K', 'x', 'K', 'K', 'R', 0xBA };
    memcpy(buf, noise, sizeof noise); n = sizeof noise;
    n += ks_frame(buf + n, KS_CMD_RF_TRACE, p, sizeof p);
    TEST_ASSERT_EQ(feed_all(&rx, buf, n, NULL), 1, "found behind the noise");

    /* Bad CRC: dropped, the following good frame still found. */
    n = ks_frame(buf, KS_CMD_RF_TRACE, p, sizeof p);
    buf[n - 1] ^= 0x55;
    n += ks_frame(buf + n, KS_CMD_RF_TRACE, p, sizeof p);
    TEST_ASSERT_EQ(feed_all(&rx, buf, n, NULL), 1, "bad CRC refused, next one taken");

    /* Oversized announced length: refused at the header, no waiting for 4 KB. */
    const uint8_t big[] = { 'K', 'S', KS_CMD_RF_TRACE, 0x00, 0x10 };
    memcpy(buf, big, sizeof big); n = sizeof big;
    n += ks_frame(buf + n, KS_CMD_RF_TRACE, p, sizeof p);
    TEST_ASSERT_EQ(feed_all(&rx, buf, n, NULL), 1, "oversized header dropped at once");

    /* Byte-at-a-time delivery is the normal case: nothing to test beyond feed. */
}

static void test_kr_encode_is_ks_respond(void)
{
    uint8_t out[16];
    size_t n = rf_trace_kr_encode(out, sizeof out, KS_CMD_RF_TRACE, KS_STATUS_OK, NULL, 0);
    const uint8_t want[] = { 'K', 'R', 0xBA, 0x00, 0x00, 0x00, 0x00 };   /* crc8 of nothing = 0 */
    TEST_ASSERT(n == sizeof want && memcmp(out, want, n) == 0, "empty OK ack");
    const uint8_t p[] = { 1, 2, 3 };
    n = rf_trace_kr_encode(out, sizeof out, 0x04, KS_STATUS_ERR_INVALID, p, 3);
    TEST_ASSERT(n == 10 && out[3] == KS_STATUS_ERR_INVALID && out[4] == 3 && out[5] == 0
                && memcmp(out + 6, p, 3) == 0 && out[9] == ks_crc8(p, 3), "payload + CRC over the payload");
    TEST_ASSERT_EQ(rf_trace_kr_encode(out, 9, 0x04, 0, p, 3), 0, "cap too small");
    /* An ack can never be mistaken for a trace batch by the host scanner. */
    n = rf_trace_kr_encode(out, sizeof out, KS_CMD_RF_TRACE, KS_STATUS_OK, NULL, 0);
    size_t st, fl;
    TEST_ASSERT(rf_trace_scan(out, n, &st, &fl, NULL) != RF_TRACE_SCAN_FOUND, "ack is not a batch");
}

void test_rf_trace_half(void)
{
    TEST_SUITE("rf_trace_half");
    TEST_RUN(test_tx_result);
    TEST_RUN(test_observe_arc_and_plos_delta);
    TEST_RUN(test_ctl_off_does_nothing);
    TEST_RUN(test_ctl_periodic_then_auto_off);
    TEST_RUN(test_ctl_restart_takes_a_new_session);
    TEST_RUN(test_ctl_sleep_and_wake_records);
    TEST_RUN(test_ctl_periodic_stays_on_its_grid);
    TEST_RUN(test_ctl_auto_off_across_the_wrap);
    TEST_RUN(test_ctl_expiry_during_sleep);
    TEST_RUN(test_ks_rx_assembles_a_request);
    TEST_RUN(test_ks_rx_resyncs);
    TEST_RUN(test_kr_encode_is_ks_respond);
}
