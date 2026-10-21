#define _POSIX_C_SOURCE 200809L   /* open_memstream */
/* rf-scope trace format — main/comm/rf/rf_trace.{h,c}.
 *
 * The same bytes leave a half on UART0 (interleaved with console text) and
 * the dongle on CDC; the host tool decodes them in Python against the shared
 * vectors (docs/contracts/rf_trace_vectors.json). These tests pin: the byte
 * layout of each record, the frame (a KR reply of command 0xBA), the strict
 * decoder (every refusal has its own code), the resync scanner (text and
 * fake headers between frames), and the ring buffer's drop accounting. */
#include "test_framework.h"
#include "../main/comm/rf/rf_trace.h"
#include "../main/comm/cdc/cdc_binary_protocol.h"

/* ── Records ──────────────────────────────────────────────────────── */

static rf_trace_rec_t mk_tx(uint32_t t, uint8_t seq)
{
    rf_trace_rec_t r = { .kind = RF_TRACE_KIND_TX };
    r.u.tx = (rf_trace_tx_t){ .t_us = t, .emitter = RF_TRACE_EMIT_RIGHT, .seq = seq,
        .pkt_kind = 0x4, .payload_len = 6, .result = RF_TRACE_TX_MAX_RT, .arc = 15,
        .plos_delta = 3, .channel = 0x4F, .flags = RF_TRACE_TXF_EXCURSION };
    return r;
}

static rf_trace_rec_t mk_rx(uint32_t t, uint8_t seq)
{
    rf_trace_rec_t r = { .kind = RF_TRACE_KIND_RX };
    r.u.rx = (rf_trace_rx_t){ .t_us = t, .emitter = RF_TRACE_EMIT_LEFT, .seq = seq,
        .pkt_kind = 0x6, .payload_len = 8, .pipe = 1, .rpd = 1, .channel = 0x52,
        .fifo_depth = 2, .flags = RF_TRACE_RXF_DELIVERED | RF_TRACE_RXF_DUP };
    return r;
}

static rf_trace_rec_t mk_per(uint32_t t)
{
    rf_trace_rec_t r = { .kind = RF_TRACE_KIND_PERIODIC };
    r.u.periodic = (rf_trace_periodic_t){ .t_us = t, .uptime_s = 0x00012345,
        .source = RF_TRACE_EMIT_DONGLE, .state = RF_TRACE_ST_WOKE, .batt_dV = 41,
        .channel = 0x4F, .frames = 0x1234, .fails = 0x0102, .dups = 0xBEEF, .drops = 0xFFFF,
        .session = 0xCAFE0102, .stress = 0x0A0B,
        .ovf = 0x0D0C, .gap_max_ms = 0x0F0E, .settle_ms = 0x1110, .bench = 0x05 };
    return r;
}

static void test_rf_trace_tx_layout_and_roundtrip(void)
{
    rf_trace_rec_t r = mk_tx(0x11223344, 0xA7);
    uint8_t b[RF_TRACE_TX_SIZE + 4];
    memset(b, 0xEE, sizeof b);
    rf_trace_tx_encode(b, &r.u.tx);
    static const uint8_t exp[RF_TRACE_TX_SIZE] = {
        0x44, 0x33, 0x22, 0x11, 0x01, 0xA7, 0x04, 0x06, 0x01, 0x0F, 0x03, 0x4F, 0x01 };
    TEST_ASSERT_EQ(rf_trace_rec_size(RF_TRACE_KIND_TX), 13, "TX record is 13 bytes");
    TEST_ASSERT(memcmp(b, exp, sizeof exp) == 0, "TX: t_us LE, emitter, seq, kind, len, result, arc, plos, ch, flags");
    TEST_ASSERT_EQ(b[RF_TRACE_TX_SIZE], 0xEE, "TX encode writes exactly 13 bytes");
    rf_trace_tx_t d; memset(&d, 0, sizeof d);
    rf_trace_tx_decode(b, &d);
    TEST_ASSERT(memcmp(&d, &r.u.tx, sizeof d) == 0, "TX round trip");
}

static void test_rf_trace_rx_layout_and_roundtrip(void)
{
    rf_trace_rec_t r = mk_rx(0xA0B0C0D0, 0x05);
    uint8_t b[RF_TRACE_RX_SIZE + 4];
    memset(b, 0xEE, sizeof b);
    rf_trace_rx_encode(b, &r.u.rx);
    static const uint8_t exp[RF_TRACE_RX_SIZE] = {
        0xD0, 0xC0, 0xB0, 0xA0, 0x00, 0x05, 0x06, 0x08, 0x01, 0x01, 0x52, 0x02, 0x03 };
    TEST_ASSERT_EQ(rf_trace_rec_size(RF_TRACE_KIND_RX), 13, "RX record is 13 bytes");
    TEST_ASSERT(memcmp(b, exp, sizeof exp) == 0, "RX: t_us LE, emitter, seq, kind, len, pipe, rpd, ch, fifo, flags");
    TEST_ASSERT_EQ(b[RF_TRACE_RX_SIZE], 0xEE, "RX encode writes exactly 13 bytes");
    rf_trace_rx_t d; memset(&d, 0, sizeof d);
    rf_trace_rx_decode(b, &d);
    TEST_ASSERT(memcmp(&d, &r.u.rx, sizeof d) == 0, "RX round trip");
}

static void test_rf_trace_periodic_layout_and_roundtrip(void)
{
    rf_trace_rec_t r = mk_per(0x01020304);
    uint8_t b[RF_TRACE_PERIODIC_SIZE + 4];
    memset(b, 0xEE, sizeof b);
    rf_trace_periodic_encode(b, &r.u.periodic);
    static const uint8_t exp[RF_TRACE_PERIODIC_SIZE] = {
        0x04, 0x03, 0x02, 0x01, 0x45, 0x23, 0x01, 0x00, 0x02, 0x02, 41, 0x4F,
        0x34, 0x12, 0x02, 0x01, 0xEF, 0xBE, 0xFF, 0xFF,
        0x02, 0x01, 0xFE, 0xCA, 0x0B, 0x0A,
        0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x05 };
    TEST_ASSERT_EQ(RF_TRACE_VERSION, 3, "format version 3: the periodic record grew (ovf, gap_max_ms, settle_ms, bench)");
    TEST_ASSERT_EQ(rf_trace_rec_size(RF_TRACE_KIND_PERIODIC), 33, "periodic record is 33 bytes");
    TEST_ASSERT(memcmp(b, exp, sizeof exp) == 0,
                "periodic: t_us, uptime_s, source, state, batt, ch, 4 × u16 LE, session u32, stress u16, "
                "ovf u16, gap_max_ms u16, settle_ms u16, bench u8");
    TEST_ASSERT_EQ(b[RF_TRACE_PERIODIC_SIZE], 0xEE, "periodic encode writes exactly 33 bytes");
    rf_trace_periodic_t d; memset(&d, 0, sizeof d);
    rf_trace_periodic_decode(b, &d);
    TEST_ASSERT(memcmp(&d, &r.u.periodic, sizeof d) == 0, "periodic round trip");
    TEST_ASSERT_EQ(rf_trace_rec_size(0), 0, "kind 0 has no size");
    TEST_ASSERT_EQ(rf_trace_rec_size(4), 0, "kind 4 has no size");
}

/* ── Frames ───────────────────────────────────────────────────────── */

static bool rec_eq(const rf_trace_rec_t *a, const rf_trace_rec_t *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case RF_TRACE_KIND_TX:       return memcmp(&a->u.tx, &b->u.tx, sizeof a->u.tx) == 0;
    case RF_TRACE_KIND_RX:       return memcmp(&a->u.rx, &b->u.rx, sizeof a->u.rx) == 0;
    case RF_TRACE_KIND_PERIODIC: return memcmp(&a->u.periodic, &b->u.periodic, sizeof a->u.periodic) == 0;
    default: return false;
    }
}

static size_t build(uint8_t *buf, uint8_t kind, size_t count)
{
    rf_trace_rec_t rec[RF_TRACE_BATCH_MAX];
    for (size_t i = 0; i < count; i++)
        rec[i] = kind == RF_TRACE_KIND_TX ? mk_tx(1000u * i, (uint8_t)(250 + i))
               : kind == RF_TRACE_KIND_RX ? mk_rx(7u * i, (uint8_t)i)
               : mk_per(0xFFFFFF00u + (uint32_t)i);
    return rf_trace_frame_encode(buf, RF_TRACE_FRAME_MAX, kind, rec, count);
}

static void test_rf_trace_frame_roundtrip(void)
{
    static const uint8_t kinds[3] = { RF_TRACE_KIND_TX, RF_TRACE_KIND_RX, RF_TRACE_KIND_PERIODIC };
    static const size_t counts[3] = { 1, 5, RF_TRACE_BATCH_MAX };
    for (int k = 0; k < 3; k++) {
        for (int c = 0; c < 3; c++) {
            uint8_t buf[RF_TRACE_FRAME_MAX];
            rf_trace_rec_t rec[RF_TRACE_BATCH_MAX];
            for (size_t i = 0; i < counts[c]; i++)
                rec[i] = kinds[k] == RF_TRACE_KIND_TX ? mk_tx(1000u * i, (uint8_t)(250 + i))
                       : kinds[k] == RF_TRACE_KIND_RX ? mk_rx(7u * i, (uint8_t)i)
                       : mk_per(0xFFFFFF00u + (uint32_t)i);
            size_t n = rf_trace_frame_encode(buf, sizeof buf, kinds[k], rec, counts[c]);
            size_t plen = 3 + counts[c] * rf_trace_rec_size(kinds[k]);
            TEST_ASSERT_EQ(n, 6 + plen + 1, "frame length = KR header + payload + CRC");
            TEST_ASSERT(buf[0] == 0x4B && buf[1] == 0x52, "frame starts with KR");
            TEST_ASSERT_EQ(buf[2], KS_CMD_RF_TRACE, "command byte is KS_CMD_RF_TRACE");
            TEST_ASSERT_EQ(buf[3], KS_STATUS_OK, "status OK");
            TEST_ASSERT_EQ(buf[4] | (buf[5] << 8), plen, "len LE = payload length");
            TEST_ASSERT_EQ(buf[6], RF_TRACE_VERSION, "version byte");
            TEST_ASSERT_EQ(buf[7], kinds[k], "kind byte");
            TEST_ASSERT_EQ(buf[8], counts[c], "count byte");
            TEST_ASSERT_EQ(buf[n - 1], ks_crc8(buf + 6, (uint16_t)plen), "CRC = ks_crc8(payload), as any KR reply");

            rf_trace_frame_t f;
            TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_OK, "frame decodes");
            TEST_ASSERT_EQ(f.kind, kinds[k], "decoded kind");
            TEST_ASSERT_EQ(f.count, counts[c], "decoded count");
            bool all = true;
            for (size_t i = 0; i < counts[c]; i++) all = all && rec_eq(&f.rec[i], &rec[i]);
            TEST_ASSERT(all, "every record survives the frame round trip");
        }
    }
}

static void test_rf_trace_encode_refusals(void)
{
    uint8_t buf[RF_TRACE_FRAME_MAX];
    rf_trace_rec_t rec[RF_TRACE_BATCH_MAX + 1];
    for (size_t i = 0; i <= RF_TRACE_BATCH_MAX; i++) rec[i] = mk_tx((uint32_t)i, (uint8_t)i);
    TEST_ASSERT_EQ(rf_trace_frame_encode(buf, sizeof buf, RF_TRACE_KIND_TX, rec, 0), 0, "count 0 refused");
    TEST_ASSERT_EQ(rf_trace_frame_encode(buf, sizeof buf, RF_TRACE_KIND_TX, rec, RF_TRACE_BATCH_MAX + 1), 0,
                   "count > BATCH_MAX refused");
    TEST_ASSERT_EQ(rf_trace_frame_encode(buf, sizeof buf, 9, rec, 1), 0, "unknown kind refused");
    TEST_ASSERT_EQ(rf_trace_frame_encode(buf, sizeof buf, RF_TRACE_KIND_RX, rec, 1), 0,
                   "a TX record in an RX frame refused");
    TEST_ASSERT_EQ(rf_trace_frame_encode(buf, 6 + 3 + 13, RF_TRACE_KIND_TX, rec, 1), 0,
                   "one byte short of room refused");
    TEST_ASSERT_EQ(rf_trace_frame_encode(buf, 6 + 3 + 13 + 1, RF_TRACE_KIND_TX, rec, 1), 23,
                   "exactly enough room accepted");
}

static void test_rf_trace_crc_flip_refused(void)
{
    uint8_t buf[RF_TRACE_FRAME_MAX];
    size_t n = build(buf, RF_TRACE_KIND_RX, 3);
    rf_trace_frame_t f;
    int crc_refusals = 0, other_refusals = 0;
    for (size_t i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[i] ^= (uint8_t)(1u << bit);
            rf_trace_err_t e = rf_trace_frame_decode(buf, n, &f);
            if (i >= RF_TRACE_HDR) crc_refusals += (e == RF_TRACE_ERR_CRC);
            else                   other_refusals += (e != RF_TRACE_OK);
            buf[i] ^= (uint8_t)(1u << bit);
        }
    }
    TEST_ASSERT_EQ(crc_refusals, (int)(n - RF_TRACE_HDR) * 8,
                   "every bit flipped in the records or the CRC → ERR_CRC");
    TEST_ASSERT_EQ(other_refusals, RF_TRACE_HDR * 8, "every bit flipped in the header → refused");
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_OK, "restored frame decodes again");
}

static void test_rf_trace_truncated_refused(void)
{
    uint8_t buf[RF_TRACE_FRAME_MAX + 1];
    size_t n = build(buf, RF_TRACE_KIND_PERIODIC, 2);
    rf_trace_frame_t f;
    int shorts = 0;
    for (size_t l = 0; l < n; l++) shorts += (rf_trace_frame_decode(buf, l, &f) == RF_TRACE_ERR_SHORT);
    TEST_ASSERT_EQ(shorts, (int)n, "every truncation (0 .. n-1 bytes) → ERR_SHORT");
    buf[n] = 0x00;
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n + 1, &f), RF_TRACE_ERR_TRAILING, "a byte after the CRC → ERR_TRAILING");
    TEST_ASSERT_EQ(rf_trace_frame_decode(NULL, n, &f), RF_TRACE_ERR_ARG, "NULL buffer → ERR_ARG");
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, NULL), RF_TRACE_ERR_ARG, "NULL output → ERR_ARG");
}

/* Re-seal a hand-edited frame: fix len and CRC so that only the field
 * under test is wrong. */
static size_t reseal(uint8_t *buf, size_t plen)
{
    buf[4] = (uint8_t)plen; buf[5] = (uint8_t)(plen >> 8);
    buf[6 + plen] = ks_crc8(buf + 6, (uint16_t)plen);
    return 6 + plen + 1;
}

static void test_rf_trace_count_bound_refused(void)
{
    /* A well-sealed frame of 17 TX records: consistent length, valid CRC —
     * only the count bound can refuse it. It is what an overflow of the
     * decoder's rec[RF_TRACE_BATCH_MAX] array would need. */
    uint8_t buf[6 + 3 + 17 * RF_TRACE_TX_SIZE + 1];
    buf[0] = 0x4B; buf[1] = 0x52; buf[2] = RF_TRACE_CMD; buf[3] = 0;
    buf[6] = RF_TRACE_VERSION; buf[7] = RF_TRACE_KIND_TX; buf[8] = 17;
    for (int i = 0; i < 17; i++) { rf_trace_rec_t r = mk_tx((uint32_t)i, (uint8_t)i); rf_trace_tx_encode(buf + 9 + i * 13, &r.u.tx); }
    size_t n = reseal(buf, 3 + 17 * 13);
    /* Spill room behind the frame: without the bound, the decoder writes a
     * 17th record past rec[] — here into `spill`, so the assertions below
     * report it instead of a smashed stack. */
    struct { rf_trace_frame_t f; rf_trace_rec_t spill[RF_TRACE_BATCH_MAX]; } g;
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &g.f), RF_TRACE_ERR_COUNT, "17 records (sealed) → ERR_COUNT");
    size_t s = 0, fl = 0;
    TEST_ASSERT_EQ(rf_trace_scan(buf, n, &s, &fl, &g.f), RF_TRACE_SCAN_NONE, "the scanner does not accept it either");

    buf[8] = 0;
    n = reseal(buf, 3);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &g.f), RF_TRACE_ERR_COUNT, "0 records (sealed) → ERR_COUNT");
}

static void test_rf_trace_field_refusals(void)
{
    uint8_t buf[RF_TRACE_FRAME_MAX];
    rf_trace_frame_t f;
    size_t n;

    n = build(buf, RF_TRACE_KIND_TX, 2); buf[0] = 0x4A;
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_MAGIC, "not K → ERR_MAGIC");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[1] = 0x53;
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_MAGIC, "a KS request is not a trace frame");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[2] = KS_CMD_RF_STATUS;
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_CMD, "a KR RF_STATUS reply → ERR_CMD");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[3] = 0x01;
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_STATUS, "status != 0 → ERR_STATUS");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[6] = RF_TRACE_VERSION + 1; n = reseal(buf, n - 7);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_VERSION, "next version (sealed) → ERR_VERSION");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[6] = 1; n = reseal(buf, n - 7);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_VERSION, "version 1 (a pre-stress firmware, sealed) → ERR_VERSION");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[6] = 2; n = reseal(buf, n - 7);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_VERSION, "version 2 (a pre-settle firmware, sealed) → ERR_VERSION");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[7] = 7; n = reseal(buf, n - 7);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_KIND, "kind 7 (sealed) → ERR_KIND");
    /* TX and RX records have the same size: a kind swap is caught by nothing
     * but the kind itself — and decodes as the OTHER kind. Periodic is not. */
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[7] = RF_TRACE_KIND_PERIODIC; n = reseal(buf, n - 7);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_LEN, "TX records announced periodic (sealed) → ERR_LEN");
    n = build(buf, RF_TRACE_KIND_TX, 2); buf[8] = 3; n = reseal(buf, n - 7);
    TEST_ASSERT_EQ(rf_trace_frame_decode(buf, n, &f), RF_TRACE_ERR_LEN, "count 3 over 2 records (sealed) → ERR_LEN");
}

/* ── Resync scanner ───────────────────────────────────────────────── */

static void test_rf_trace_scan_through_text_and_fake_header(void)
{
    uint8_t s[1024]; size_t n = 0;
    static const char t1[] = "I (1234) kbd: HB up=12 s → vetos=usb é\r\n\x1b[0;32mK R ok\x1b[0m\r\n";
    memcpy(s + n, t1, sizeof t1 - 1); n += sizeof t1 - 1;
    size_t f1 = n;
    n += build(s + n, RF_TRACE_KIND_TX, 3);
    size_t f1_end = n;
    /* Binary noise: a fake header whose plausible length (one RX record,
     * 16 bytes of payload) swallows the start of the real frame that
     * follows. A scanner that skips a refused candidate by its claimed
     * length loses frame 2. */
    static const uint8_t fake[] = { 'K', 'R', RF_TRACE_CMD, 0x00, 16, 0, RF_TRACE_VERSION, RF_TRACE_KIND_RX, 1, 0xAA };
    memcpy(s + n, fake, sizeof fake); n += sizeof fake;
    size_t f2 = n;
    n += build(s + n, RF_TRACE_KIND_PERIODIC, 2);
    static const char t2[] = "W (99) rf: MAX_RT\r\nKR";
    memcpy(s + n, t2, sizeof t2 - 1); n += sizeof t2 - 1;

    rf_trace_frame_t f; size_t st = 0, fl = 0, off = 0;
    TEST_ASSERT_EQ(rf_trace_scan(s, n, &st, &fl, &f), RF_TRACE_SCAN_FOUND, "frame 1 found behind console text");
    TEST_ASSERT_EQ(st, f1, "frame 1 at its offset");
    TEST_ASSERT_EQ(f.kind, RF_TRACE_KIND_TX, "frame 1 is TX");
    off = st + fl;
    TEST_ASSERT_EQ(off, f1_end, "frame 1 length");
    TEST_ASSERT_EQ(rf_trace_scan(s + off, n - off, &st, &fl, &f), RF_TRACE_SCAN_FOUND,
                   "frame 2 found behind a fake header that overlaps it");
    TEST_ASSERT_EQ(off + st, f2, "frame 2 at its offset");
    TEST_ASSERT_EQ(f.kind, RF_TRACE_KIND_PERIODIC, "frame 2 is periodic");
    TEST_ASSERT_EQ(f.count, 2, "frame 2 count");
    off += st + fl;
    TEST_ASSERT_EQ(rf_trace_scan(s + off, n - off, &st, &fl, &f), RF_TRACE_SCAN_PARTIAL,
                   "a trailing \"KR\" may begin a frame: partial");
    TEST_ASSERT_EQ(off + st, n - 2, "keep the last two bytes");
}

static void test_rf_trace_scan_streaming(void)
{
    uint8_t s[RF_TRACE_FRAME_MAX];
    size_t n = build(s, RF_TRACE_KIND_RX, 4);
    rf_trace_frame_t f; size_t st = 9, fl = 9;
    for (size_t l = 1; l < n; l++) {
        if (rf_trace_scan(s, l, &st, &fl, &f) != RF_TRACE_SCAN_PARTIAL || st != 0) {
            TEST_ASSERT(false, "every prefix of a frame is PARTIAL at offset 0");
            return;
        }
    }
    TEST_ASSERT(true, "every prefix of a frame is PARTIAL at offset 0");
    TEST_ASSERT_EQ(rf_trace_scan(s, n, &st, &fl, &f), RF_TRACE_SCAN_FOUND, "the whole frame is FOUND");
    TEST_ASSERT_EQ(fl, n, "its full length");

    static const uint8_t text[] = "plain text, no frame\r\n";
    TEST_ASSERT_EQ(rf_trace_scan(text, sizeof text - 1, &st, &fl, &f), RF_TRACE_SCAN_NONE, "text only → NONE");
    TEST_ASSERT_EQ(st, sizeof text - 1, "all of it can be dropped");
    TEST_ASSERT_EQ(rf_trace_scan(text, 0, &st, &fl, &f), RF_TRACE_SCAN_NONE, "empty → NONE");

    /* A header that can never become a frame must not stall the stream:
     * count 200 is refused from the 9 header bytes, without waiting for
     * the 2600 bytes its length would announce. */
    uint8_t bad[9 + RF_TRACE_FRAME_MAX] = { 'K', 'R', RF_TRACE_CMD, 0, 0x28, 0x0A, 1, 1, 200 };
    size_t m = build(bad + 9, RF_TRACE_KIND_TX, 1);
    TEST_ASSERT_EQ(rf_trace_scan(bad, 9 + m, &st, &fl, &f), RF_TRACE_SCAN_FOUND, "a refused header does not stall the scan");
    TEST_ASSERT_EQ(st, 9, "the frame right after it is found");
}

/* ── Ring buffer ──────────────────────────────────────────────────── */

static void test_rf_trace_ring(void)
{
    rf_trace_rec_t store[8], out[RF_TRACE_BATCH_MAX];
    rf_trace_ring_t r;
    TEST_ASSERT(!rf_trace_ring_init(&r, store, 6), "capacity 6 (not a power of two) refused");
    TEST_ASSERT(!rf_trace_ring_init(&r, store, 0), "capacity 0 refused");
    TEST_ASSERT(!rf_trace_ring_init(&r, NULL, 8), "no storage refused");
    TEST_ASSERT(rf_trace_ring_init(&r, store, 8), "capacity 8 accepted");
    TEST_ASSERT_EQ(rf_trace_ring_pop_batch(&r, out, 4), 0, "empty ring pops nothing");
    TEST_ASSERT_EQ(rf_trace_ring_peek_kind(&r), 0, "empty ring: peek says 0");

    for (uint8_t i = 0; i < 8; i++) { rf_trace_rec_t x = mk_tx(i, i); TEST_ASSERT(rf_trace_ring_push(&r, &x), "push while room"); }
    TEST_ASSERT_EQ(rf_trace_ring_count(&r), 8, "full");
    int refused = 0;
    for (uint8_t i = 0; i < 5; i++) { rf_trace_rec_t x = mk_tx(100, 100); refused += !rf_trace_ring_push(&r, &x); }
    TEST_ASSERT_EQ(refused, 5, "push on a full ring returns false, never blocks");
    TEST_ASSERT_EQ(rf_trace_ring_count(&r), 8, "a refused push does not overwrite");
    TEST_ASSERT_EQ(rf_trace_ring_drops_take(&r), 5, "5 drops counted");
    TEST_ASSERT_EQ(rf_trace_ring_drops_take(&r), 0, "read-and-reset");

    TEST_ASSERT_EQ(rf_trace_ring_pop_batch(&r, out, 3), 3, "batch bounded by max");
    TEST_ASSERT(out[0].u.tx.seq == 0 && out[2].u.tx.seq == 2, "FIFO order");
    { rf_trace_rec_t x = mk_tx(1, 1); rf_trace_ring_push(&r, &x); rf_trace_ring_push(&r, &x); rf_trace_ring_push(&r, &x); }
    TEST_ASSERT_EQ(rf_trace_ring_drops_take(&r), 0, "room again: no drop");
    { rf_trace_rec_t x = mk_tx(1, 1); rf_trace_ring_push(&r, &x); }
    TEST_ASSERT_EQ(rf_trace_ring_drops_take(&r), 1, "full again: one drop");

    /* Kind boundary: a batch stops at the first record of another kind. */
    TEST_ASSERT(rf_trace_ring_init(&r, store, 8), "re-init");
    rf_trace_rec_t a = mk_rx(1, 1), p = mk_per(2);
    rf_trace_ring_push(&r, &a); rf_trace_ring_push(&r, &a); rf_trace_ring_push(&r, &p); rf_trace_ring_push(&r, &a);
    TEST_ASSERT_EQ(rf_trace_ring_peek_kind(&r), RF_TRACE_KIND_RX, "peek: the oldest record's kind");
    TEST_ASSERT_EQ(rf_trace_ring_count(&r), 4, "peek pops nothing");
    TEST_ASSERT_EQ(rf_trace_ring_pop_batch(&r, out, 16), 2, "batch = the run of same-kind records");
    TEST_ASSERT_EQ(rf_trace_ring_peek_kind(&r), RF_TRACE_KIND_PERIODIC, "peek follows the tail");
    TEST_ASSERT_EQ(rf_trace_ring_pop_batch(&r, out, 16), 1, "then the periodic alone");
    TEST_ASSERT_EQ(out[0].kind, RF_TRACE_KIND_PERIODIC, "periodic popped");
    TEST_ASSERT_EQ(rf_trace_ring_pop_batch(&r, out, 16), 1, "then the last RX");

    /* Wrap: many laps, order preserved, and across the u32 index wrap. */
    TEST_ASSERT(rf_trace_ring_init(&r, store, 8), "re-init");
    r.head = r.tail = 0xFFFFFFF0u;
    bool ok = true; uint8_t next = 0, want = 0;
    for (int lap = 0; lap < 50; lap++) {
        for (int i = 0; i < 5; i++) { rf_trace_rec_t x = mk_tx(0, next++); ok = ok && rf_trace_ring_push(&r, &x); }
        size_t k = rf_trace_ring_pop_batch(&r, out, 5);
        ok = ok && k == 5;
        for (size_t i = 0; i < k; i++) ok = ok && out[i].u.tx.seq == want++;
    }
    TEST_ASSERT(ok, "250 records through an 8-slot ring across the u32 wrap, in order");
    TEST_ASSERT_EQ(rf_trace_ring_count(&r), 0, "drained");
    TEST_ASSERT_EQ(rf_trace_ring_drops_take(&r), 0, "no drop when the consumer keeps up");
}

/* ── Shared vectors (docs/contracts/rf_trace_vectors.json) ─────────── */

/* scripts/gen/rf_trace_vectors.c, compiled with RF_TRACE_VECTORS_NO_MAIN. */
extern void rf_trace_vectors_write(FILE *out);

static void test_rf_trace_vectors_json(void)
{
    char *gen = NULL; size_t gen_len = 0;
    FILE *m = open_memstream(&gen, &gen_len);
    TEST_ASSERT(m != NULL, "memstream");
    if (!m) return;
    rf_trace_vectors_write(m);
    fclose(m);

    FILE *f = fopen(RF_TRACE_VECTORS_JSON, "rb");
    TEST_ASSERT(f != NULL, "docs/contracts/rf_trace_vectors.json exists");
    if (!f) { free(gen); return; }
    static char committed[65536];
    size_t n = fread(committed, 1, sizeof committed, f);
    fclose(f);
    bool same = n == gen_len && memcmp(committed, gen, n) == 0;
    TEST_ASSERT(same, "committed rf_trace_vectors.json == what the C codec produces "
                      "(regenerate: scripts/gen_rf_trace_vectors.sh)");
    free(gen);
}

void test_rf_trace(void)
{
    printf("\n-- rf-scope trace format (main/comm/rf/rf_trace.h) --\n");
    test_rf_trace_tx_layout_and_roundtrip();
    test_rf_trace_rx_layout_and_roundtrip();
    test_rf_trace_periodic_layout_and_roundtrip();
    test_rf_trace_frame_roundtrip();
    test_rf_trace_encode_refusals();
    test_rf_trace_crc_flip_refused();
    test_rf_trace_truncated_refused();
    test_rf_trace_count_bound_refused();
    test_rf_trace_field_refusals();
    test_rf_trace_scan_through_text_and_fake_header();
    test_rf_trace_scan_streaming();
    test_rf_trace_ring();
    test_rf_trace_vectors_json();
}
