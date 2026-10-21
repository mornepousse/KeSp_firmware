/* Generates docs/contracts/rf_trace_vectors.json from the real codec of
 * main/comm/rf/rf_trace.c (host build). Run scripts/gen_rf_trace_vectors.sh.
 *
 * Linked twice: as a program by the script, and into test/test_runner with
 * RF_TRACE_VECTORS_NO_MAIN, where test_rf_trace compares its output with the
 * committed file byte for byte. The host tool's pytest
 * (tools/rf_scope/tests/test_codec.py) decodes every vector and must find
 * the same fields: the C and Python codecs cannot drift apart silently. */
#include <stdio.h>
#include <string.h>
#include "rf_trace.h"
#include "cdc_binary_protocol.h"

static void hex(FILE *o, const uint8_t *b, size_t n)
{
    fputc('"', o);
    for (size_t i = 0; i < n; i++) fprintf(o, "%s%02x", i ? " " : "", b[i]);
    fputc('"', o);
}

static void rec_json(FILE *o, const rf_trace_rec_t *r)
{
    switch (r->kind) {
    case RF_TRACE_KIND_TX: {
        const rf_trace_tx_t *x = &r->u.tx;
        fprintf(o, "{\"t_us\": %u, \"emitter\": %u, \"seq\": %u, \"pkt_kind\": %u, \"payload_len\": %u, "
                   "\"result\": %u, \"arc\": %u, \"plos_delta\": %u, \"channel\": %u, \"flags\": %u}",
                (unsigned)x->t_us, x->emitter, x->seq, x->pkt_kind, x->payload_len,
                x->result, x->arc, x->plos_delta, x->channel, x->flags);
        break; }
    case RF_TRACE_KIND_RX: {
        const rf_trace_rx_t *x = &r->u.rx;
        fprintf(o, "{\"t_us\": %u, \"emitter\": %u, \"seq\": %u, \"pkt_kind\": %u, \"payload_len\": %u, "
                   "\"pipe\": %u, \"rpd\": %u, \"channel\": %u, \"fifo_depth\": %u, \"flags\": %u}",
                (unsigned)x->t_us, x->emitter, x->seq, x->pkt_kind, x->payload_len,
                x->pipe, x->rpd, x->channel, x->fifo_depth, x->flags);
        break; }
    default: {
        const rf_trace_periodic_t *x = &r->u.periodic;
        fprintf(o, "{\"t_us\": %u, \"uptime_s\": %u, \"source\": %u, \"state\": %u, \"batt_dV\": %u, "
                   "\"channel\": %u, \"frames\": %u, \"fails\": %u, \"dups\": %u, \"drops\": %u, "
                   "\"session\": %u, \"stress\": %u, \"ovf\": %u, \"gap_max_ms\": %u, "
                   "\"settle_ms\": %u, \"bench\": %u}",
                (unsigned)x->t_us, (unsigned)x->uptime_s, x->source, x->state, x->batt_dV,
                x->channel, x->frames, x->fails, x->dups, x->drops, (unsigned)x->session, x->stress,
                x->ovf, x->gap_max_ms, x->settle_ms, x->bench);
        break; }
    }
}

static const char *kind_name(uint8_t k)
{
    return k == RF_TRACE_KIND_TX ? "tx" : k == RF_TRACE_KIND_RX ? "rx" : "periodic";
}

static rf_trace_rec_t tx(uint32_t t, uint8_t em, uint8_t seq, uint8_t pk, uint8_t len,
                         uint8_t res, uint8_t arc, uint8_t plos, uint8_t ch, uint8_t fl)
{
    rf_trace_rec_t r = { .kind = RF_TRACE_KIND_TX };
    r.u.tx = (rf_trace_tx_t){ t, em, seq, pk, len, res, arc, plos, ch, fl };
    return r;
}

static rf_trace_rec_t rx(uint32_t t, uint8_t em, uint8_t seq, uint8_t pk, uint8_t len,
                         uint8_t pipe, uint8_t rpd, uint8_t ch, uint8_t fifo, uint8_t fl)
{
    rf_trace_rec_t r = { .kind = RF_TRACE_KIND_RX };
    r.u.rx = (rf_trace_rx_t){ t, em, seq, pk, len, pipe, rpd, ch, fifo, fl };
    return r;
}

static rf_trace_rec_t per(uint32_t t, uint32_t up, uint8_t src, uint8_t st, uint8_t batt,
                          uint8_t ch, uint16_t frames, uint16_t fails, uint16_t dups, uint16_t drops,
                          uint32_t session, uint16_t stress,
                          uint16_t ovf, uint16_t gap_max_ms, uint16_t settle_ms, uint8_t bench)
{
    rf_trace_rec_t r = { .kind = RF_TRACE_KIND_PERIODIC };
    r.u.periodic = (rf_trace_periodic_t){ t, up, src, st, batt, ch, frames, fails, dups, drops, session, stress,
                                          ovf, gap_max_ms, settle_ms, bench };
    return r;
}

/* One accepted frame: name, hex, kind, decoded records. */
static void frame_vec(FILE *o, bool first, const char *name, uint8_t kind,
                      const rf_trace_rec_t *rec, size_t n)
{
    uint8_t b[RF_TRACE_FRAME_MAX];
    size_t len = rf_trace_frame_encode(b, sizeof b, kind, rec, n);
    fprintf(o, "%s    {\n      \"name\": \"%s\",\n      \"hex\": ", first ? "" : ",\n", name);
    hex(o, b, len);
    fprintf(o, ",\n      \"version\": %u,\n      \"kind\": \"%s\",\n      \"records\": [\n",
            RF_TRACE_VERSION, kind_name(kind));
    for (size_t i = 0; i < n; i++) {
        fputs("        ", o);
        rec_json(o, &rec[i]);
        fputs(i + 1 < n ? ",\n" : "\n", o);
    }
    fputs("      ]\n    }", o);
}

static void refused_vec(FILE *o, bool first, const char *name, const uint8_t *b, size_t n)
{
    /* Spill room: a refused vector must never be decoded past rec[], but
     * if the count bound is ever lost the generator must not crash. */
    static struct { rf_trace_frame_t f; rf_trace_rec_t spill[RF_TRACE_BATCH_MAX]; } g;
    rf_trace_err_t e = rf_trace_frame_decode(b, n, &g.f);
    fprintf(o, "%s    {\"name\": \"%s\", \"error\": \"%s\", \"hex\": ", first ? "" : ",\n",
            name, rf_trace_err_name(e));
    hex(o, b, n);
    fputc('}', o);
}

static size_t reseal(uint8_t *b, size_t plen)
{
    b[4] = (uint8_t)plen; b[5] = (uint8_t)(plen >> 8);
    b[6 + plen] = ks_crc8(b + 6, (uint16_t)plen);
    return 6 + plen + 1;
}

void rf_trace_vectors_write(FILE *o)
{
    const rf_trace_rec_t tx1[] = {
        tx(0x00012345u, RF_TRACE_EMIT_LEFT, 0x00, 0x4, 6, RF_TRACE_TX_ACK, 0, 0, 0x52, 0) };
    const rf_trace_rec_t tx3[] = {
        tx(1000000u, RF_TRACE_EMIT_LEFT,  0xFE, 0x4, 6, RF_TRACE_TX_ACK,     1, 0, 0x52, RF_TRACE_TXF_EXCURSION),
        tx(1000850u, RF_TRACE_EMIT_LEFT,  0xFF, 0x6, 8, RF_TRACE_TX_MAX_RT, 15, 2, 0x52, RF_TRACE_TXF_EXCURSION),
        tx(1003000u, RF_TRACE_EMIT_RIGHT, 0x00, 0x4, 6, RF_TRACE_TX_TIMEOUT, 0, 0, 0x4F, 0) };
    rf_trace_rec_t tx16[RF_TRACE_BATCH_MAX];
    for (unsigned i = 0; i < RF_TRACE_BATCH_MAX; i++)   /* t_us crosses the u32 wrap */
        tx16[i] = tx(0xFFFFF000u + i * 0x200u, RF_TRACE_EMIT_RIGHT, (uint8_t)(0xF8 + i), 0x4, 6,
                     RF_TRACE_TX_ACK, (uint8_t)(i % 4), 0, 0x52, 0);
    const rf_trace_rec_t rx1[] = {
        rx(0x89ABCDEFu, RF_TRACE_EMIT_RIGHT, 0x7F, 0x4, 6, 1, 1, 0x52, 1, RF_TRACE_RXF_DELIVERED) };
    const rf_trace_rec_t rx4[] = {
        rx(5000u, RF_TRACE_EMIT_LEFT,  10, 0x4, 6, 1, 1, 0x52, 1, RF_TRACE_RXF_DELIVERED),
        rx(5010u, RF_TRACE_EMIT_LEFT,  10, 0x4, 6, 1, 0, 0x52, 2, RF_TRACE_RXF_DUP),
        rx(5900u, RF_TRACE_EMIT_RIGHT, 200, 0x6, 8, 1, 1, 0x52, 1, 0),
        rx(6100u, RF_TRACE_EMIT_MOUSE, 3, 0x5, 6, 2, 1, 0x53, 3, RF_TRACE_RXF_DELIVERED) };
    const rf_trace_rec_t per1[] = {
        per(42000000u, 42u, RF_TRACE_EMIT_LEFT, RF_TRACE_ST_SLEEPING, 39, 0x52, 120, 3, 0, 0, 1u, 0, 0, 0, 0, 0) };
    const rf_trace_rec_t per2[] = {
        per(0xFFFFFFFFu, 4295u, RF_TRACE_EMIT_DONGLE, RF_TRACE_ST_AWAKE, 0, 0x52, 65535, 0, 17, 9, 0xFFFFFFFFu, 480, 3, 12, 41, 0),
        per(0x000F4240u, 4296u, RF_TRACE_EMIT_RIGHT,  RF_TRACE_ST_WOKE, 41, 0x4F, 1, 1, 0, 65535, 7u, 65535, 0, 0, 0, 0x01) };
    const rf_trace_rec_t per3[] = {   /* v3: a starved, bench-configured dongle (every new field at its edge) */
        per(1234567u, 1u, RF_TRACE_EMIT_DONGLE, RF_TRACE_ST_AWAKE, 0, 0x52, 300, 0, 0, 0, 2u, 0,
            65535, 65535, 1000, 0x07) };
    const rf_trace_rec_t rx_noseq[] = {
        rx(7000u, RF_TRACE_EMIT_LEFT,  0, 0x5, 9, 0, 1, 0x52, 1, RF_TRACE_RXF_DELIVERED | RF_TRACE_RXF_NOSEQ),
        rx(7400u, RF_TRACE_EMIT_RIGHT, 0x2A, 0xA, 6, 0, 1, 0x52, 1, 0) };

    fputs("{\n", o);
    fputs("  \"comment\": \"rf-scope trace frames generated by main/comm/rf/rf_trace.c (scripts/gen_rf_trace_vectors.sh). "
          "Pinned by test/test_rf_trace.c and tools/rf_scope/tests/test_codec.py. Do not edit by hand.\",\n", o);
    fprintf(o, "  \"format\": {\"magic\": \"4b 52\", \"cmd\": %u, \"status\": 0, \"version\": %u, \"batch_max\": %u, "
               "\"crc\": \"ks_crc8: poly 0x31, init 0x00, MSB-first, over the payload (version..records)\", "
               "\"record_sizes\": {\"tx\": %u, \"rx\": %u, \"periodic\": %u}, "
               "\"kinds\": {\"tx\": %u, \"rx\": %u, \"periodic\": %u}},\n",
            RF_TRACE_CMD, RF_TRACE_VERSION, RF_TRACE_BATCH_MAX,
            RF_TRACE_TX_SIZE, RF_TRACE_RX_SIZE, RF_TRACE_PERIODIC_SIZE,
            RF_TRACE_KIND_TX, RF_TRACE_KIND_RX, RF_TRACE_KIND_PERIODIC);

    fputs("  \"frames\": [\n", o);
    frame_vec(o, true,  "tx_single_ack",                 RF_TRACE_KIND_TX, tx1, 1);
    frame_vec(o, false, "tx_seq_wrap_maxrt_timeout",     RF_TRACE_KIND_TX, tx3, 3);
    frame_vec(o, false, "tx_full_batch_t_us_wrap",       RF_TRACE_KIND_TX, tx16, RF_TRACE_BATCH_MAX);
    frame_vec(o, false, "rx_single_delivered",           RF_TRACE_KIND_RX, rx1, 1);
    frame_vec(o, false, "rx_dup_and_mouse",              RF_TRACE_KIND_RX, rx4, 4);
    frame_vec(o, false, "rx_noseq_hidreport_and_stress", RF_TRACE_KIND_RX, rx_noseq, 2);
    frame_vec(o, false, "periodic_left_going_to_sleep",  RF_TRACE_KIND_PERIODIC, per1, 1);
    frame_vec(o, false, "periodic_dongle_and_right_woke", RF_TRACE_KIND_PERIODIC, per2, 2);
    frame_vec(o, false, "periodic_dongle_v3_health_and_bench", RF_TRACE_KIND_PERIODIC, per3, 1);
    fputs("\n  ],\n", o);

    /* Refused: each one wrong in exactly one way (the others re-sealed). */
    uint8_t good[RF_TRACE_FRAME_MAX], b[RF_TRACE_FRAME_MAX + 32];
    size_t gn = rf_trace_frame_encode(good, sizeof good, RF_TRACE_KIND_TX, tx3, 3);
    size_t n;
    fputs("  \"refused\": [\n", o);
    memcpy(b, good, gn); b[gn - 1] ^= 0x01;
    refused_vec(o, true, "crc_flipped", b, gn);
    memcpy(b, good, gn); b[12] ^= 0x80;
    refused_vec(o, false, "record_bit_flipped", b, gn);
    refused_vec(o, false, "truncated_by_one", good, gn - 1);
    refused_vec(o, false, "header_only", good, RF_TRACE_HDR);
    memcpy(b, good, gn); b[gn] = 0x0A;
    refused_vec(o, false, "trailing_byte", b, gn + 1);
    memcpy(b, good, gn); b[2] = KS_CMD_RF_STATUS;
    refused_vec(o, false, "other_kr_command", b, gn);
    memcpy(b, good, gn); b[1] = KS_MAGIC_1;
    refused_vec(o, false, "ks_request_magic", b, gn);
    memcpy(b, good, gn); b[3] = 0x01;
    refused_vec(o, false, "status_not_ok", b, gn);
    memcpy(b, good, gn); b[6] = RF_TRACE_VERSION + 1; n = reseal(b, gn - 7);
    refused_vec(o, false, "version_next_sealed", b, n);
    memcpy(b, good, gn); b[6] = 1; n = reseal(b, gn - 7);
    refused_vec(o, false, "version_1_sealed", b, n);
    memcpy(b, good, gn); b[6] = 2; n = reseal(b, gn - 7);
    refused_vec(o, false, "version_2_sealed", b, n);
    memcpy(b, good, gn); b[7] = 0; n = reseal(b, gn - 7);
    refused_vec(o, false, "kind_0_sealed", b, n);
    memcpy(b, good, gn); b[8] = 2; n = reseal(b, gn - 7);
    refused_vec(o, false, "count_disagrees_with_len_sealed", b, n);
    memcpy(b, good, gn); b[8] = 0; n = reseal(b, 3);
    refused_vec(o, false, "count_0_sealed", b, n);
    {   /* 17 TX records, sealed: only the count bound refuses it. */
        static uint8_t big[RF_TRACE_HDR + 17 * RF_TRACE_TX_SIZE + 1];
        memcpy(big, good, RF_TRACE_HDR);
        big[8] = 17;
        for (unsigned i = 0; i < 17; i++) rf_trace_tx_encode(big + RF_TRACE_HDR + i * RF_TRACE_TX_SIZE, &tx16[i % 16].u.tx);
        n = reseal(big, 3 + 17 * RF_TRACE_TX_SIZE);
        refused_vec(o, false, "count_17_sealed", big, n);
    }
    fputs("\n  ],\n", o);

    /* Stream: console text, a frame, a fake header overlapping the next
     * frame, a frame, text, and a dangling "KR". */
    uint8_t s[1024]; size_t sn = 0, at[2], ln[2];
    static const char t1[] = "I (1234) kbd: HB up=12 s \xe2\x86\x92 vetos=usb \xc3\xa9\r\n\x1b[0;32mK R ok\x1b[0m\r\n";
    memcpy(s, t1, sizeof t1 - 1); sn = sizeof t1 - 1;
    at[0] = sn; ln[0] = rf_trace_frame_encode(s + sn, sizeof s - sn, RF_TRACE_KIND_TX, tx3, 3); sn += ln[0];
    static const uint8_t fake[] = { 'K', 'R', RF_TRACE_CMD, 0x00, 16, 0, RF_TRACE_VERSION, RF_TRACE_KIND_RX, 1, 0xAA };
    memcpy(s + sn, fake, sizeof fake); sn += sizeof fake;
    at[1] = sn; ln[1] = rf_trace_frame_encode(s + sn, sizeof s - sn, RF_TRACE_KIND_PERIODIC, per2, 2); sn += ln[1];
    static const char t2[] = "W (99) rf: MAX_RT\r\nKR";
    memcpy(s + sn, t2, sizeof t2 - 1); sn += sizeof t2 - 1;
    fputs("  \"stream\": {\n    \"hex\": ", o);
    hex(o, s, sn);
    fprintf(o, ",\n    \"frames_at\": [[%zu, %zu], [%zu, %zu]],\n    \"keep_tail\": 2\n  }\n}\n",
            at[0], ln[0], at[1], ln[1]);
}

#ifndef RF_TRACE_VECTORS_NO_MAIN
#include "tinyusb_cdc_acm.h"
/* cdc_binary_protocol.c is linked for ks_crc8 only; its CDC writes are never
 * called here. */
esp_err_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf, const uint8_t *buf, size_t size)
{ (void)itf; (void)buf; (void)size; return 0; }
esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf, uint32_t timeout_ticks)
{ (void)itf; (void)timeout_ticks; return 0; }
bool ks_tx_reserve(uint32_t need) { (void)need; return true; }   /* cdc_internal.h; never called here */

int main(void)
{
    rf_trace_vectors_write(stdout);
    return 0;
}
#endif
