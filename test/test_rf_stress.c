/* rf-scope stress generator — main/comm/rf/rf_stress.{h,c} (pure).
 *
 * Pins the KS_CMD_RF_STRESS (0xBB) request payload, the stress frame, and the
 * scheduler: given the pattern and the "TX done" events, when the next frame
 * is due, when the run stops on its own (count, duration, the cap of a
 * count-only run), what a stop does, and that a radio that refuses to
 * transmit never turns MAX into a busy loop. Times are µs on a fake clock. */
#include "test_framework.h"
#include "../main/comm/rf/rf_stress.h"
#include "../main/comm/rf/rf_packet.h"
#include "../main/comm/cdc/cdc_binary_protocol.h"

#define MS 1000ull
#define S  1000000ull

static size_t start_payload(uint8_t *b, uint8_t pattern, uint16_t rate, uint32_t count, uint16_t dur,
                            uint8_t burst_len, uint16_t pause_ms)
{
    b[0] = RF_STRESS_SUB_START; b[1] = pattern;
    b[2] = (uint8_t)rate; b[3] = (uint8_t)(rate >> 8);
    b[4] = (uint8_t)count; b[5] = (uint8_t)(count >> 8); b[6] = (uint8_t)(count >> 16); b[7] = (uint8_t)(count >> 24);
    b[8] = (uint8_t)dur; b[9] = (uint8_t)(dur >> 8);
    b[10] = burst_len;
    b[11] = (uint8_t)pause_ms; b[12] = (uint8_t)(pause_ms >> 8);
    return 13;
}

static rf_stress_params_t params(uint8_t pattern, uint16_t rate, uint32_t count, uint16_t dur,
                                 uint8_t burst_len, uint16_t pause_ms)
{
    uint8_t b[16]; rf_stress_cmd_t c;
    size_t n = start_payload(b, pattern, rate, count, dur, burst_len, pause_ms);
    rf_stress_err_t e = rf_stress_parse(b, (uint16_t)n, &c);
    TEST_ASSERT_EQ(e, RF_STRESS_OK, "test params parse");
    return c.p;
}

/* ── Command id and payload ─────────────────────────────────────────── */

static void test_stress_cmd_id_and_parse(void)
{
    TEST_ASSERT_EQ(KS_CMD_RF_STRESS, 0xBB, "RF_STRESS is 0xBB, next to RF_TRACE");
    TEST_ASSERT_EQ(RF_STRESS_CMD, KS_CMD_RF_STRESS, "one id");

    uint8_t b[16]; rf_stress_cmd_t c;
    size_t n = start_payload(b, RF_STRESS_RATE, 250, 0x01020304u, 90, 0, 0);
    TEST_ASSERT_EQ(n, RF_STRESS_START_LEN, "start is 13 bytes");
    TEST_ASSERT(RF_STRESS_START_LEN <= 16, "fits the UART0 reader's 16-byte payload");
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_OK, "rate start accepted");
    TEST_ASSERT(c.start, "a start");
    TEST_ASSERT_EQ(c.p.pattern, RF_STRESS_RATE, "pattern");
    TEST_ASSERT_EQ(c.p.rate_hz, 250, "rate u16 LE");
    TEST_ASSERT_EQ(c.p.count, 0x01020304u, "count u32 LE");
    TEST_ASSERT_EQ(c.p.duration_s, 90, "duration u16 LE");

    const uint8_t stop[] = { RF_STRESS_SUB_STOP };
    TEST_ASSERT_EQ(rf_stress_parse(stop, 1, &c), RF_STRESS_OK, "stop accepted");
    TEST_ASSERT(!c.start, "a stop");

    /* Defaults and caps: a run ALWAYS ends on its own. */
    n = start_payload(b, RF_STRESS_MAX, 0, 0, 0, 0, 0);
    rf_stress_parse(b, (uint16_t)n, &c);
    TEST_ASSERT_EQ(c.p.duration_s, RF_STRESS_DEFAULT_S, "neither count nor duration: default duration");
    n = start_payload(b, RF_STRESS_MAX, 0, 1000, 0, 0, 0);
    rf_stress_parse(b, (uint16_t)n, &c);
    TEST_ASSERT_EQ(c.p.duration_s, RF_STRESS_COUNT_CAP_S, "count only: capped in time as well");
    n = start_payload(b, RF_STRESS_HELD, 0, 0, 3600, 0, 0);
    rf_stress_parse(b, (uint16_t)n, &c);
    TEST_ASSERT_EQ(c.p.duration_s, 3600, "duration kept");
    n = start_payload(b, RF_STRESS_BURST, 0, 0, 10, 8, 0);
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_OK, "burst with no pause = back to back");

    /* Refusals */
    TEST_ASSERT_EQ(rf_stress_parse(NULL, 0, &c), RF_STRESS_ERR_LEN, "empty");
    const uint8_t stop2[] = { RF_STRESS_SUB_STOP, 0 };
    TEST_ASSERT_EQ(rf_stress_parse(stop2, 2, &c), RF_STRESS_ERR_LEN, "stop + trailing byte");
    n = start_payload(b, RF_STRESS_RATE, 10, 0, 10, 0, 0);
    TEST_ASSERT_EQ(rf_stress_parse(b, 12, &c), RF_STRESS_ERR_LEN, "start one byte short");
    TEST_ASSERT_EQ(rf_stress_parse(b, 14, &c), RF_STRESS_ERR_LEN, "start one byte long");
    const uint8_t bad_sub[] = { 0x02 };
    TEST_ASSERT_EQ(rf_stress_parse(bad_sub, 1, &c), RF_STRESS_ERR_SUB, "unknown sub-command");
    n = start_payload(b, 4, 10, 0, 10, 0, 0);
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_ERR_PATTERN, "unknown pattern");
    n = start_payload(b, RF_STRESS_RATE, 0, 0, 10, 0, 0);
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_ERR_RANGE, "RATE at 0 Hz");
    n = start_payload(b, RF_STRESS_RATE, RF_STRESS_RATE_MAX_HZ + 1, 0, 10, 0, 0);
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_ERR_RANGE, "RATE above the max");
    n = start_payload(b, RF_STRESS_RATE, RF_STRESS_RATE_MAX_HZ, 0, 10, 0, 0);
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_OK, "RATE at the max");
    n = start_payload(b, RF_STRESS_BURST, 0, 0, 10, 0, 100);
    TEST_ASSERT_EQ(rf_stress_parse(b, (uint16_t)n, &c), RF_STRESS_ERR_RANGE, "BURST of 0 frames");
}

/* ── The frame ──────────────────────────────────────────────────────── */

static void test_stress_frame(void)
{
    uint8_t f[RF_STRESS_FRAME_LEN + 2];
    memset(f, 0xEE, sizeof f);
    TEST_ASSERT_EQ(rf_stress_frame(f, sizeof f, RF_HALF_RIGHT, 0x5C), RF_STRESS_FRAME_LEN, "frame length");
    TEST_ASSERT_EQ(RF_STRESS_FRAME_LEN, 2 + RF_HALF_BITMAP_BYTES, "the size of a MATRIX frame");
    TEST_ASSERT_EQ(f[0], (PKT_TYPE_STRESS << 4) | RF_HALF_RIGHT, "[0xA0 | half]");
    TEST_ASSERT_EQ(f[1], 0x5C, "[seq]");
    TEST_ASSERT(f[2] == 0 && f[3] == 0 && f[4] == 0 && f[5] == 0, "padding zero");
    TEST_ASSERT_EQ(f[RF_STRESS_FRAME_LEN], 0xEE, "writes exactly the frame");
    TEST_ASSERT_EQ(rf_packet_type(f, RF_STRESS_FRAME_LEN), PKT_TYPE_STRESS, "kind STRESS, never MATRIX");
    TEST_ASSERT_EQ(rf_stress_frame(f, RF_STRESS_FRAME_LEN - 1, RF_HALF_LEFT, 0), 0, "too small: nothing");
    rf_stress_frame(f, sizeof f, RF_HALF_LEFT, 0);
    TEST_ASSERT_EQ(f[0], 0xA0, "left: 0xA0");
}

/* ── Scheduler ──────────────────────────────────────────────────────── */

/* Runs the scheduler on a fake clock: every SEND takes `tx_us` and ends in
 * `res`. Records the start time of each frame; returns the frame count. */
static size_t drive(rf_stress_sched_t *s, uint64_t *t, uint64_t until, uint64_t tx_us,
                    rf_stress_tx_t res, uint64_t *starts, size_t max)
{
    size_t n = 0, zero_waits = 0;
    for (int guard = 0; guard < 200000 && *t <= until; guard++) {
        uint64_t w = 0;
        rf_stress_step_t st = rf_stress_step(s, *t, &w);
        if (st == RF_STRESS_IDLE) break;
        if (st == RF_STRESS_WAIT) {
            if (w == 0) zero_waits++;
            *t += w;
            continue;
        }
        if (n < max) starts[n] = *t;
        n++;
        uint32_t sess = s->session;
        *t += tx_us;
        rf_stress_tx_done(s, sess, *t, res);
    }
    TEST_ASSERT_EQ(zero_waits, 0, "a WAIT is never 0 (the glue would spin)");
    return n;
}

static void test_sched_rate_on_a_grid(void)
{
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_RATE, 100, 0, 2, 0, 0);
    uint64_t t = 1 * S, starts[300];
    rf_stress_start(&s, &p, t);
    uint64_t w;
    TEST_ASSERT_EQ(rf_stress_step(&s, t, &w), RF_STRESS_SEND, "first frame at once");
    size_t n = drive(&s, &t, 10 * S, 700, RF_STRESS_TX_ACK, starts, 300);
    TEST_ASSERT_EQ(n, 200, "100 Hz for 2 s: 200 frames");
    TEST_ASSERT_EQ(starts[1] - starts[0], 10 * MS, "10 ms apart");
    TEST_ASSERT_EQ(starts[199] - starts[0], 1990 * MS, "on the grid: no drift from the TX time");
    TEST_ASSERT(!s.on, "stopped by itself");
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_DURATION, "duration over");
    TEST_ASSERT_EQ(s.sent, 200, "sent counter");
    TEST_ASSERT_EQ(s.acked, 200, "acked counter");
}

static void test_sched_rate_late_tx_does_not_catch_up_in_a_storm(void)
{
    /* 500 Hz asked, every TX takes 4 ms (MAX_RT with retries): the frames go
     * back to back at the TX pace + the minimum gap, never a burst to
     * "catch up" the missed grid points. */
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_RATE, 500, 0, 1, 0, 0);
    uint64_t t = 0, starts[600];
    rf_stress_start(&s, &p, t);
    size_t n = drive(&s, &t, 5 * S, 4 * MS, RF_STRESS_TX_REFUSED, starts, 600);
    for (size_t i = 1; i < n && i < 600; i++)
        TEST_ASSERT(starts[i] - starts[i - 1] >= 4 * MS + RF_STRESS_MIN_GAP_US, "no catch-up burst");
    TEST_ASSERT(n >= 230 && n <= 250, "~1 s / 4.2 ms of frames");
    TEST_ASSERT_EQ(s.refused, n, "refused counted");
}

static void test_sched_max_back_to_back_with_a_minimum_gap(void)
{
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_MAX, 0, 100, 0, 0, 0);
    uint64_t t = 0, starts[200];
    rf_stress_start(&s, &p, t);
    size_t n = drive(&s, &t, 60 * S, 600, RF_STRESS_TX_ACK, starts, 200);
    TEST_ASSERT_EQ(n, 100, "count reached: 100 frames");
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_COUNT, "stopped by the count");
    TEST_ASSERT_EQ(starts[1] - starts[0], 600 + RF_STRESS_MIN_GAP_US,
                   "next frame when the previous TX completed + the minimum gap (the task blocks, IDLE runs)");
    /* After the TX: WAIT the gap, not SEND — the glue must block. */
    rf_stress_init(&s); rf_stress_start(&s, &p, 0);
    uint64_t w;
    TEST_ASSERT_EQ(rf_stress_step(&s, 0, &w), RF_STRESS_SEND, "send");
    rf_stress_tx_done(&s, s.session, 600, RF_STRESS_TX_ACK);
    TEST_ASSERT_EQ(rf_stress_step(&s, 600, &w), RF_STRESS_WAIT, "then wait");
    TEST_ASSERT_EQ(w, RF_STRESS_MIN_GAP_US, "the minimum gap");
}

static void test_sched_burst(void)
{
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_BURST, 0, 0, 1, 5, 200);
    uint64_t t = 0, starts[100];
    rf_stress_start(&s, &p, t);
    size_t n = drive(&s, &t, 10 * S, 500, RF_STRESS_TX_ACK, starts, 100);
    TEST_ASSERT_EQ(starts[1] - starts[0], 500 + RF_STRESS_MIN_GAP_US, "inside a burst: back to back");
    TEST_ASSERT_EQ(starts[4] - starts[3], 500 + RF_STRESS_MIN_GAP_US, "5th frame still in the burst");
    TEST_ASSERT_EQ(starts[5] - starts[4], 500 + 200 * MS, "then the pause, counted from the last TX's end");
    TEST_ASSERT_EQ(starts[10] - starts[9], 500 + 200 * MS, "every burst");
    /* 1 s: a burst of 5 every 203.3 ms (4 × 0.7 + 0.5 + 200) → 5 bursts (0 … 813.2 ms) */
    TEST_ASSERT_EQ(n, 25, "5 bursts of 5 in 1 s");
}

static void test_sched_held_mimics_the_reassertion(void)
{
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_HELD, 9999, 0, 3, 0, 0);
    uint64_t t = 0, starts[100];
    rf_stress_start(&s, &p, t);
    size_t n = drive(&s, &t, 10 * S, 700, RF_STRESS_TX_ACK, starts, 100);
    TEST_ASSERT_EQ(starts[1] - starts[0], RF_STRESS_HELD_PERIOD_MS * MS, "every 100 ms, the rate ignored");
    TEST_ASSERT_EQ(RF_STRESS_HELD_PERIOD_MS, 100, "the halves' re-assertion period");
    TEST_ASSERT_EQ(n, 30, "3 s at 10 Hz");
}

static void test_sched_unavailable_radio_backs_off(void)
{
    /* The radio refuses to transmit (lock held, asleep): nothing went out, so
     * no seq is consumed and the count does not move — and MAX must not spin. */
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_MAX, 0, 10, 0, 0, 0);
    rf_stress_start(&s, &p, 0);
    uint64_t w;
    TEST_ASSERT_EQ(rf_stress_step(&s, 0, &w), RF_STRESS_SEND, "send");
    uint8_t seq = s.seq;
    rf_stress_tx_done(&s, s.session, 5, RF_STRESS_TX_UNAVAIL);
    TEST_ASSERT_EQ(s.seq, seq, "seq not consumed");
    TEST_ASSERT_EQ(s.sent, 0, "not sent");
    TEST_ASSERT_EQ(s.unavail, 1, "counted unavailable");
    TEST_ASSERT_EQ(rf_stress_step(&s, 5, &w), RF_STRESS_WAIT, "backs off");
    TEST_ASSERT_EQ(w, RF_STRESS_BACKOFF_US, "by the back-off");
    /* A frame that went out consumes its seq, refused or not. */
    rf_stress_step(&s, 5 + RF_STRESS_BACKOFF_US, &w);
    rf_stress_tx_done(&s, s.session, 6 + RF_STRESS_BACKOFF_US, RF_STRESS_TX_REFUSED);
    TEST_ASSERT_EQ(s.seq, (uint8_t)(seq + 1), "refused frame: seq consumed");
    TEST_ASSERT_EQ(s.sent, 1, "refused frame: sent");
    /* A radio that never comes back: the run still ends (count-only cap). */
    uint64_t t = 10 * MS, starts[4];
    drive(&s, &t, (RF_STRESS_COUNT_CAP_S + 5) * S, 0, RF_STRESS_TX_UNAVAIL, starts, 4);
    TEST_ASSERT(!s.on, "ends by the cap");
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_DURATION, "duration cap");
}

static void test_sched_seq_wraps_and_counts_until_the_end(void)
{
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_MAX, 0, 300, 0, 0, 0);
    rf_stress_start(&s, &p, 0);
    TEST_ASSERT_EQ(s.seq, 0, "seq starts at 0");
    uint64_t t = 0, starts[4];
    drive(&s, &t, 60 * S, 100, RF_STRESS_TX_ACK, starts, 4);
    TEST_ASSERT_EQ(s.sent, 300, "300 sent");
    TEST_ASSERT_EQ(s.seq, (uint8_t)300, "8-bit seq wrapped");
}

static void test_sched_stop_restart_and_session(void)
{
    rf_stress_sched_t s; rf_stress_init(&s);
    uint64_t w;
    TEST_ASSERT_EQ(rf_stress_step(&s, 0, &w), RF_STRESS_IDLE, "off at init: IDLE (block forever)");
    TEST_ASSERT_EQ(rf_stress_remaining_s(&s, 0), 0, "off: nothing remaining");
    rf_stress_params_t p = params(RF_STRESS_RATE, 10, 0, 30, 0, 0);
    rf_stress_start(&s, &p, 0);
    uint32_t s1 = s.session;
    TEST_ASSERT(s1 != 0, "a session number");
    TEST_ASSERT_EQ(rf_stress_remaining_s(&s, 0), 30, "30 s remaining at start");
    TEST_ASSERT_EQ(rf_stress_remaining_s(&s, 29 * S + 1), 1, "rounded up");
    rf_stress_step(&s, 0, &w);
    rf_stress_stop(&s, RF_STRESS_STOP_CMD);
    TEST_ASSERT(!s.on, "stopped");
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_CMD, "by the command");
    TEST_ASSERT_EQ(rf_stress_step(&s, 1, &w), RF_STRESS_IDLE, "IDLE after a stop");
    /* A TX that completes after the stop changes nothing. */
    rf_stress_tx_done(&s, s1, 2, RF_STRESS_TX_ACK);
    TEST_ASSERT_EQ(s.sent, 0, "late TX done after the stop: ignored");
    rf_stress_stop(&s, RF_STRESS_STOP_SLEEP);
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_CMD, "a stop while off changes nothing");

    rf_stress_start(&s, &p, 10 * S);
    uint32_t s2 = s.session;
    TEST_ASSERT(s2 != s1 && s2 != 0, "a new session per start");
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_NONE, "fresh run");
    TEST_ASSERT_EQ(s.seq, 0, "seq restarts");
    rf_stress_tx_done(&s, s1, 10 * S + 1, RF_STRESS_TX_ACK);
    TEST_ASSERT_EQ(s.sent, 0, "a TX of the PREVIOUS session does not count in this one");
    rf_stress_stop(&s, RF_STRESS_STOP_SLEEP);
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_SLEEP, "stopped by the sleep");

    /* Restart while running: the new run replaces the old one. */
    rf_stress_start(&s, &p, 20 * S);
    uint32_t s3 = s.session;
    rf_stress_start(&s, &p, 21 * S);
    TEST_ASSERT(s.session != s3, "restart = new session");
    TEST_ASSERT_EQ(rf_stress_remaining_s(&s, 21 * S), 30, "restart: the clock from now");

    /* Session numbering skips 0. */
    s.last_session = 0xFFFFFFFFu;
    rf_stress_start(&s, &p, 0);
    TEST_ASSERT_EQ(s.session, 1, "after 0xFFFFFFFF: 1, never 0");
}

static void test_sched_wait_never_overshoots_the_end(void)
{
    /* A burst pause (5 s) longer than what is left of the run (1 s): the task
     * wakes at the END, not after the pause — a stopped run never lingers. */
    rf_stress_sched_t s; rf_stress_init(&s);
    rf_stress_params_t p = params(RF_STRESS_BURST, 0, 0, 1, 1, 5000);
    rf_stress_start(&s, &p, 0);
    uint64_t w;
    TEST_ASSERT_EQ(rf_stress_step(&s, 0, &w), RF_STRESS_SEND, "send");
    rf_stress_tx_done(&s, s.session, 500, RF_STRESS_TX_ACK);
    TEST_ASSERT_EQ(rf_stress_step(&s, 500, &w), RF_STRESS_WAIT, "wait");
    TEST_ASSERT_EQ(w, 1 * S - 500, "until the end of the run, not the end of the pause");
    TEST_ASSERT_EQ(rf_stress_step(&s, 1 * S, &w), RF_STRESS_IDLE, "over: IDLE");
    TEST_ASSERT_EQ(s.stopped_by, RF_STRESS_STOP_DURATION, "by its duration");
}

void test_rf_stress(void)
{
    TEST_SUITE("rf_stress");
    TEST_RUN(test_stress_cmd_id_and_parse);
    TEST_RUN(test_stress_frame);
    TEST_RUN(test_sched_rate_on_a_grid);
    TEST_RUN(test_sched_rate_late_tx_does_not_catch_up_in_a_storm);
    TEST_RUN(test_sched_max_back_to_back_with_a_minimum_gap);
    TEST_RUN(test_sched_burst);
    TEST_RUN(test_sched_held_mimics_the_reassertion);
    TEST_RUN(test_sched_unavailable_radio_backs_off);
    TEST_RUN(test_sched_seq_wraps_and_counts_until_the_end);
    TEST_RUN(test_sched_stop_restart_and_session);
    TEST_RUN(test_sched_wait_never_overshoots_the_end);
}
