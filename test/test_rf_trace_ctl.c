/* rf-scope trace control — main/comm/rf/rf_trace_ctl.{h,c}.
 *
 * Pins the KS_CMD_RF_TRACE (0xBA) request payload, the auto-off deadline
 * (across the u32 millisecond wrap), the per-emitter duplicate detector, the
 * identity read from a received frame, the FIFO depth estimate, and the rule
 * that a synthetic stress frame is dropped before the fusion engine. */
#include "test_framework.h"
#include "../main/comm/rf/rf_trace_ctl.h"
#include "../main/comm/rf/rf_trace.h"
#include "../main/comm/rf/rf_packet.h"

/* ── Command payload ─────────────────────────────────────────────────── */

static void test_rf_trace_ctl_parse(void)
{
    rf_trace_ctl_cmd_t c;

    const uint8_t stop[] = { RF_TRACE_SUB_STOP };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(stop, 1, &c), RF_TRACE_CTL_OK, "stop accepted");
    TEST_ASSERT(!c.start, "stop is not a start");

    const uint8_t start_def[] = { RF_TRACE_SUB_START };
    c.duration_s = 7;
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_def, 1, &c), RF_TRACE_CTL_OK, "bare start accepted");
    TEST_ASSERT(c.start, "bare start is a start");
    TEST_ASSERT_EQ(c.duration_s, RF_TRACE_DEFAULT_S, "bare start → default duration");

    const uint8_t start_120[] = { RF_TRACE_SUB_START, 120, 0 };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_120, 3, &c), RF_TRACE_CTL_OK, "start(120) accepted");
    TEST_ASSERT_EQ(c.duration_s, 120, "duration little-endian low byte");

    const uint8_t start_le[] = { RF_TRACE_SUB_START, 0x10, 0x0E };   /* 3600 */
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_le, 3, &c), RF_TRACE_CTL_OK, "start(3600) accepted");
    TEST_ASSERT_EQ(c.duration_s, 3600, "duration is u16 LE");

    const uint8_t start_max[] = { RF_TRACE_SUB_START, 0xFF, 0xFF };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_max, 3, &c), RF_TRACE_CTL_OK, "start(65535) accepted");
    TEST_ASSERT_EQ(c.duration_s, 65535, "max duration kept");

    const uint8_t start_zero[] = { RF_TRACE_SUB_START, 0, 0 };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_zero, 3, &c), RF_TRACE_CTL_OK, "start(0) accepted");
    TEST_ASSERT_EQ(c.duration_s, RF_TRACE_DEFAULT_S, "start(0) → default duration");

    /* Refusals */
    TEST_ASSERT_EQ(rf_trace_ctl_parse(NULL, 0, &c), RF_TRACE_CTL_ERR_LEN, "empty payload refused");
    TEST_ASSERT_EQ(rf_trace_ctl_parse(NULL, 3, &c), RF_TRACE_CTL_ERR_LEN, "NULL payload refused");
    const uint8_t stop_extra[] = { RF_TRACE_SUB_STOP, 0 };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(stop_extra, 2, &c), RF_TRACE_CTL_ERR_LEN, "stop with trailing byte refused");
    const uint8_t start_2[] = { RF_TRACE_SUB_START, 5 };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_2, 2, &c), RF_TRACE_CTL_ERR_LEN, "start with half a u16 refused");
    const uint8_t start_4[] = { RF_TRACE_SUB_START, 5, 0, 0 };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(start_4, 4, &c), RF_TRACE_CTL_ERR_LEN, "start with trailing byte refused");
    const uint8_t bad_sub[] = { 0x02, 5, 0 };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(bad_sub, 3, &c), RF_TRACE_CTL_ERR_SUB, "unknown sub-command refused");
    const uint8_t bad_sub1[] = { 0xFF };
    TEST_ASSERT_EQ(rf_trace_ctl_parse(bad_sub1, 1, &c), RF_TRACE_CTL_ERR_SUB, "0xFF sub-command refused");
}

/* ── Auto-off deadline ──────────────────────────────────────────────── */

static void test_rf_trace_ctl_timer(void)
{
    rf_trace_timer_t t = {0};
    TEST_ASSERT(!rf_trace_timer_expired(&t, 0), "never started: not expired");

    rf_trace_timer_start(&t, 1000, 10);
    TEST_ASSERT(t.on, "started");
    TEST_ASSERT(!rf_trace_timer_expired(&t, 1000), "at start: not expired");
    TEST_ASSERT(!rf_trace_timer_expired(&t, 10999), "1 ms before: not expired");
    TEST_ASSERT(rf_trace_timer_expired(&t, 11000), "at the deadline: expired");
    TEST_ASSERT(rf_trace_timer_expired(&t, 50000), "after: expired");

    /* Across the u32 wrap: started 2 s before the wrap, 10 s duration. */
    rf_trace_timer_start(&t, 0xFFFFFFFFu - 1999u, 10);
    TEST_ASSERT(!rf_trace_timer_expired(&t, 0xFFFFFFFFu), "just before the wrap: not expired");
    TEST_ASSERT(!rf_trace_timer_expired(&t, 0), "at the wrap: not expired");
    TEST_ASSERT(!rf_trace_timer_expired(&t, 7999), "8 s after the wrap - 1 ms: not expired");
    TEST_ASSERT(rf_trace_timer_expired(&t, 8000), "10 s elapsed across the wrap: expired");

    /* Max duration: 65535 s does not overflow the ms arithmetic. */
    rf_trace_timer_start(&t, 5, 65535);
    TEST_ASSERT(!rf_trace_timer_expired(&t, 5u + 65535000u - 1u), "max duration - 1 ms: not expired");
    TEST_ASSERT(rf_trace_timer_expired(&t, 5u + 65535000u), "max duration: expired");

    /* Restart with a new duration replaces the old deadline. */
    rf_trace_timer_start(&t, 0, 1);
    rf_trace_timer_start(&t, 500, 10);
    TEST_ASSERT(!rf_trace_timer_expired(&t, 1500), "restart: the old 1 s deadline is gone");
    TEST_ASSERT(rf_trace_timer_expired(&t, 10500), "restart: the new deadline counts from the restart");

    rf_trace_timer_stop(&t);
    TEST_ASSERT(!t.on, "stopped");
    TEST_ASSERT(!rf_trace_timer_expired(&t, 999999), "stopped: never expired");
}

/* ── Duplicate detector ─────────────────────────────────────────────── */

static void test_rf_trace_ctl_dup(void)
{
    rf_trace_dup_t d;
    rf_trace_dup_reset(&d);

    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_LEFT, 0), "first frame of an emitter (seq 0): not a dup");
    TEST_ASSERT(rf_trace_dup_check(&d, RF_TRACE_EMIT_LEFT, 0), "same seq again: dup");
    TEST_ASSERT(rf_trace_dup_check(&d, RF_TRACE_EMIT_LEFT, 0), "a third copy: still dup");
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_LEFT, 1), "next seq: not a dup");

    /* Per emitter: the right's seq 1 is not the left's seq 1. */
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_RIGHT, 1), "other emitter, same seq: not a dup");
    TEST_ASSERT(rf_trace_dup_check(&d, RF_TRACE_EMIT_LEFT, 1), "left's previous seq unchanged by the right");
    TEST_ASSERT(rf_trace_dup_check(&d, RF_TRACE_EMIT_RIGHT, 1), "right repeats: dup");

    /* Only the PREVIOUS frame counts: 5, 6, 5 is a reorder/rewind, not a dup. */
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_MOUSE, 5), "mouse 5");
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_MOUSE, 6), "mouse 6");
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_MOUSE, 5), "mouse 5 again after 6: not a dup");

    /* The 8-bit wrap: 255 → 0 is a new frame. */
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_DONGLE, 255), "255");
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_DONGLE, 0), "255 → 0: not a dup");

    /* Out of range emitter: never a dup, no out-of-bounds write. */
    TEST_ASSERT(!rf_trace_dup_check(&d, 4, 9), "emitter 4: not a dup");
    TEST_ASSERT(!rf_trace_dup_check(&d, 4, 9), "emitter 4 twice: still not a dup");
    TEST_ASSERT(!rf_trace_dup_check(&d, 0xFF, 9), "emitter 255: not a dup");

    /* Reset forgets everything (a restarted trace). */
    rf_trace_dup_reset(&d);
    TEST_ASSERT(!rf_trace_dup_check(&d, RF_TRACE_EMIT_LEFT, 1), "after reset: not a dup");
}

/* ── Frame identity ─────────────────────────────────────────────────── */

static void test_rf_trace_ctl_ident(void)
{
    rf_trace_ident_t id;
    uint8_t buf[32];

    rf_matrix_t m = { .half = RF_HALF_RIGHT, .bitmap = {1, 2, 3, 4}, .seq = 0x42 };
    uint16_t n = rf_encode_matrix(buf, &m);
    rf_trace_frame_ident(buf, n, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_MATRIX, "matrix kind");
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_RIGHT, "matrix right → emitter RIGHT");
    TEST_ASSERT(id.has_seq, "matrix carries a seq");
    TEST_ASSERT_EQ(id.seq, 0x42, "matrix seq");

    m.half = RF_HALF_LEFT; m.seq = 7;
    n = rf_encode_matrix(buf, &m);
    rf_trace_frame_ident(buf, n, false, &id);
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_LEFT, "matrix left → emitter LEFT");
    TEST_ASSERT_EQ(id.seq, 7, "matrix left seq");

    rf_status_t st = { .batt_dV = 40, .seq = 0x99, .half = RF_HALF_RIGHT };
    n = rf_encode_status(buf, &st);
    rf_trace_frame_ident(buf, n, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_STATUS, "status kind");
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_RIGHT, "status right flag → RIGHT");
    TEST_ASSERT(id.has_seq, "status carries a seq");
    TEST_ASSERT_EQ(id.seq, 0x99, "status seq");

    rf_heartbeat_t h = { .bitmap = {0}, .seq = 0x31 };
    n = rf_encode_heartbeat(buf, &h);
    rf_trace_frame_ident(buf, n, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_HEARTBEAT, "heartbeat kind");
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_LEFT, "heartbeat → LEFT");
    TEST_ASSERT(id.has_seq, "heartbeat carries a seq");
    TEST_ASSERT_EQ(id.seq, 0x31, "heartbeat seq");

    const uint8_t kb[6] = { 4, 0, 0, 0, 0, 0 };
    n = rf_encode_hidreport_kbd(buf, 0, kb);
    rf_trace_frame_ident(buf, n, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_HIDREPORT, "hidreport kind");
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_LEFT, "hidreport on the keyboard slot → LEFT");
    TEST_ASSERT(!id.has_seq, "hidreport has no seq");
    TEST_ASSERT_EQ(id.seq, 0, "no seq → 0");

    /* The mouse slot is the mouse, whatever the frame says. */
    n = rf_encode_hidreport_mouse(buf, 1, 2, 3, 0);
    rf_trace_frame_ident(buf, n, true, &id);
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_MOUSE, "mouse slot → MOUSE");
    TEST_ASSERT(!id.has_seq, "mouse hidreport has no seq");

    rf_sync_req_t q = { .next = 3 };
    n = rf_encode_sync_req(buf, &q);
    rf_trace_frame_ident(buf, n, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_SYNC_REQ, "sync req kind");
    TEST_ASSERT(!id.has_seq, "sync req has no seq (byte 1 is the chunk index)");

    /* Stress: half in the low nibble, seq in byte 1. */
    buf[0] = (uint8_t)((PKT_TYPE_STRESS << 4) | RF_HALF_RIGHT); buf[1] = 0xC3;
    rf_trace_frame_ident(buf, 8, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_STRESS, "stress kind");
    TEST_ASSERT_EQ(id.emitter, RF_TRACE_EMIT_RIGHT, "stress right → RIGHT");
    TEST_ASSERT(id.has_seq, "stress carries a seq");
    TEST_ASSERT_EQ(id.seq, 0xC3, "stress seq");

    /* Truncated frames: kind kept, no seq read past the end. */
    buf[0] = (uint8_t)(PKT_TYPE_MATRIX << 4); buf[5] = 0x77;
    rf_trace_frame_ident(buf, 5, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, PKT_TYPE_MATRIX, "short matrix: kind kept");
    TEST_ASSERT(!id.has_seq, "short matrix: no seq");
    buf[0] = (uint8_t)(PKT_TYPE_STRESS << 4);
    rf_trace_frame_ident(buf, 1, false, &id);
    TEST_ASSERT(!id.has_seq, "1-byte stress: no seq");
    rf_trace_frame_ident(buf, 0, false, &id);
    TEST_ASSERT_EQ(id.pkt_kind, 0, "empty frame: kind 0");
    TEST_ASSERT(!id.has_seq, "empty frame: no seq");
}

/* ── FIFO depth ─────────────────────────────────────────────────────── */

static void test_rf_trace_ctl_fifo_depth(void)
{
    TEST_ASSERT_EQ(rf_trace_fifo_depth(1, 0), 1, "a lone frame: depth 1");
    TEST_ASSERT_EQ(rf_trace_fifo_depth(3, 0), 3, "first of 3: full");
    TEST_ASSERT_EQ(rf_trace_fifo_depth(3, 1), 2, "second of 3");
    TEST_ASSERT_EQ(rf_trace_fifo_depth(3, 2), 1, "last of 3");
    TEST_ASSERT_EQ(rf_trace_fifo_depth(7, 0), 3, "a long pass caps at the 3 hardware levels");
    TEST_ASSERT_EQ(rf_trace_fifo_depth(7, 5), 2, "near the end of a long pass");
    TEST_ASSERT_EQ(rf_trace_fifo_depth(2, 2), 0, "idx past the pass: 0, no underflow");
}

/* ── Drain pass: depth reconstructed, order kept, nothing lost ────────── */

static size_t pass_run(size_t frames, uint8_t *depth, uint8_t *seq, size_t cap)
{
    static rf_trace_rec_t store[32];
    rf_trace_ring_t ring;
    rf_trace_ring_init(&ring, store, 32);
    rf_trace_pass_t p = { .n = 0 };
    for (size_t i = 0; i < frames; i++) {
        rf_trace_rx_t r = { .seq = (uint8_t)i, .fifo_depth = 0xEE };
        rf_trace_pass_add(&p, &r, &ring);
    }
    rf_trace_pass_end(&p, &ring);
    TEST_ASSERT_EQ(p.n, 0, "pass emptied at the end");
    rf_trace_rec_t out[32];
    size_t n = 0, k;
    while ((k = rf_trace_ring_pop_batch(&ring, out + n, 32 - n)) > 0) n += k;
    for (size_t i = 0; i < n && i < cap; i++) {
        TEST_ASSERT_EQ(out[i].kind, RF_TRACE_KIND_RX, "pass pushes RX records");
        depth[i] = out[i].u.rx.fifo_depth;
        seq[i] = out[i].u.rx.seq;
    }
    return n;
}

static void test_rf_trace_ctl_pass(void)
{
    uint8_t d[32], s[32];
    TEST_ASSERT_EQ(pass_run(0, d, s, 32), 0, "empty pass: nothing pushed");

    TEST_ASSERT_EQ(pass_run(1, d, s, 32), 1, "1 frame: 1 record");
    TEST_ASSERT_EQ(d[0], 1, "1 frame: depth 1");

    TEST_ASSERT_EQ(pass_run(3, d, s, 32), 3, "3 frames: 3 records");
    TEST_ASSERT_EQ(d[0], 3, "3 frames: first read with a full FIFO");
    TEST_ASSERT_EQ(d[1], 2, "3 frames: second");
    TEST_ASSERT_EQ(d[2], 1, "3 frames: last");

    /* Longer than the pass buffer: flushed mid-pass, depth still exact. */
    size_t n = pass_run(20, d, s, 32);
    TEST_ASSERT_EQ(n, 20, "20 frames: every record pushed");
    for (size_t i = 0; i < 20; i++) {
        TEST_ASSERT_EQ(s[i], i, "20 frames: order kept");
        TEST_ASSERT_EQ(d[i], rf_trace_fifo_depth(20, i), "20 frames: depth as if seen whole");
    }
    n = pass_run(RF_TRACE_PASS_MAX + 1, d, s, 32);
    TEST_ASSERT_EQ(n, RF_TRACE_PASS_MAX + 1, "PASS_MAX+1 frames: all pushed");
    TEST_ASSERT_EQ(d[RF_TRACE_PASS_MAX - 2], 3, "PASS_MAX+1: third from last is full");
    TEST_ASSERT_EQ(d[RF_TRACE_PASS_MAX - 1], 2, "PASS_MAX+1: second from last");
    TEST_ASSERT_EQ(d[RF_TRACE_PASS_MAX], 1, "PASS_MAX+1: last");
}

/* ── Stress frames are dropped before the engine ─────────────────────── */

static void test_rf_trace_ctl_drop_stress(void)
{
    uint8_t buf[32] = {0};
    buf[0] = (uint8_t)(PKT_TYPE_STRESS << 4);
    TEST_ASSERT(rf_trace_drop_before_engine(buf, 8), "stress left: dropped");
    buf[0] = (uint8_t)((PKT_TYPE_STRESS << 4) | RF_HALF_RIGHT);
    TEST_ASSERT(rf_trace_drop_before_engine(buf, 8), "stress right: dropped");
    TEST_ASSERT(rf_trace_drop_before_engine(buf, 1), "a truncated stress frame is still dropped");

    /* Every real kind goes through. */
    static const uint8_t real[] = { PKT_TYPE_KEY, PKT_TYPE_HEARTBEAT, PKT_TYPE_TRACKPAD,
        PKT_TYPE_MATRIX, PKT_TYPE_HIDREPORT, PKT_TYPE_STATUS, PKT_TYPE_SYNC_BEACON,
        PKT_TYPE_SYNC_CHUNK, PKT_TYPE_SYNC_REQ, PKT_TYPE_PAIR_ACK, PKT_TYPE_PAIR_REQ };
    for (size_t i = 0; i < sizeof real; i++) {
        buf[0] = (uint8_t)(real[i] << 4);
        TEST_ASSERT(!rf_trace_drop_before_engine(buf, 8), "real frame kind not dropped");
    }
    TEST_ASSERT(!rf_trace_drop_before_engine(buf, 0), "empty frame: not a stress frame");
    TEST_ASSERT(!rf_trace_drop_before_engine(NULL, 8), "NULL: not a stress frame");
}

void test_rf_trace_ctl(void)
{
    TEST_SUITE("rf_trace_ctl");
    TEST_RUN(test_rf_trace_ctl_parse);
    TEST_RUN(test_rf_trace_ctl_timer);
    TEST_RUN(test_rf_trace_ctl_dup);
    TEST_RUN(test_rf_trace_ctl_ident);
    TEST_RUN(test_rf_trace_ctl_fifo_depth);
    TEST_RUN(test_rf_trace_ctl_pass);
    TEST_RUN(test_rf_trace_ctl_drop_stress);
}
