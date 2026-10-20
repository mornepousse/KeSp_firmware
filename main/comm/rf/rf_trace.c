/* rf-scope trace records — codec, strict decoder, resync scanner, ring.
 * Format and framing rationale: rf_trace.h. PURE (host-tested,
 * test/test_rf_trace.c); the only dependency is the project's CRC-8. */
#include "rf_trace.h"
#include "cdc_binary_protocol.h"   /* ks_crc8, KS_MAGIC_0, KR_MAGIC_1, KS_CMD_RF_TRACE */
#include <string.h>

_Static_assert(RF_TRACE_CMD == KS_CMD_RF_TRACE, "rf_trace frames are KR replies of KS_CMD_RF_TRACE");
_Static_assert(RF_TRACE_FRAME_MAX - RF_TRACE_KR_HDR - 1u <= 0xFFFFu, "payload length fits the KR u16");
_Static_assert(RF_TRACE_BATCH_MAX <= 0xFFu, "count is one byte");

const char *rf_trace_err_name(rf_trace_err_t e)
{
    switch (e) {
    case RF_TRACE_OK:           return "ok";
    case RF_TRACE_ERR_ARG:      return "arg";
    case RF_TRACE_ERR_SHORT:    return "short";
    case RF_TRACE_ERR_MAGIC:    return "magic";
    case RF_TRACE_ERR_CMD:      return "cmd";
    case RF_TRACE_ERR_STATUS:   return "status";
    case RF_TRACE_ERR_VERSION:  return "version";
    case RF_TRACE_ERR_KIND:     return "kind";
    case RF_TRACE_ERR_COUNT:    return "count";
    case RF_TRACE_ERR_LEN:      return "len";
    case RF_TRACE_ERR_CRC:      return "crc";
    case RF_TRACE_ERR_TRAILING: return "trailing";
    }
    return "?";
}

size_t rf_trace_rec_size(uint8_t kind)
{
    switch (kind) {
    case RF_TRACE_KIND_TX:       return RF_TRACE_TX_SIZE;
    case RF_TRACE_KIND_RX:       return RF_TRACE_RX_SIZE;
    case RF_TRACE_KIND_PERIODIC: return RF_TRACE_PERIODIC_SIZE;
    default:                     return 0;
    }
}

/* ── Little-endian helpers ── */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Records ── */

void rf_trace_tx_encode(uint8_t *o, const rf_trace_tx_t *r)
{
    put_u32(o, r->t_us);
    o[4] = r->emitter; o[5] = r->seq; o[6] = r->pkt_kind; o[7] = r->payload_len;
    o[8] = r->result; o[9] = r->arc; o[10] = r->plos_delta; o[11] = r->channel; o[12] = r->flags;
}

void rf_trace_tx_decode(const uint8_t *i, rf_trace_tx_t *r)
{
    r->t_us = get_u32(i);
    r->emitter = i[4]; r->seq = i[5]; r->pkt_kind = i[6]; r->payload_len = i[7];
    r->result = i[8]; r->arc = i[9]; r->plos_delta = i[10]; r->channel = i[11]; r->flags = i[12];
}

void rf_trace_rx_encode(uint8_t *o, const rf_trace_rx_t *r)
{
    put_u32(o, r->t_us);
    o[4] = r->emitter; o[5] = r->seq; o[6] = r->pkt_kind; o[7] = r->payload_len;
    o[8] = r->pipe; o[9] = r->rpd; o[10] = r->channel; o[11] = r->fifo_depth; o[12] = r->flags;
}

void rf_trace_rx_decode(const uint8_t *i, rf_trace_rx_t *r)
{
    r->t_us = get_u32(i);
    r->emitter = i[4]; r->seq = i[5]; r->pkt_kind = i[6]; r->payload_len = i[7];
    r->pipe = i[8]; r->rpd = i[9]; r->channel = i[10]; r->fifo_depth = i[11]; r->flags = i[12];
}

void rf_trace_periodic_encode(uint8_t *o, const rf_trace_periodic_t *r)
{
    put_u32(o, r->t_us);
    put_u32(o + 4, r->uptime_s);
    o[8] = r->source; o[9] = r->state; o[10] = r->batt_dV; o[11] = r->channel;
    put_u16(o + 12, r->frames); put_u16(o + 14, r->fails);
    put_u16(o + 16, r->dups);   put_u16(o + 18, r->drops);
    put_u32(o + 20, r->session); put_u16(o + 24, r->stress);
}

void rf_trace_periodic_decode(const uint8_t *i, rf_trace_periodic_t *r)
{
    r->t_us = get_u32(i);
    r->uptime_s = get_u32(i + 4);
    r->source = i[8]; r->state = i[9]; r->batt_dV = i[10]; r->channel = i[11];
    r->frames = get_u16(i + 12); r->fails = get_u16(i + 14);
    r->dups = get_u16(i + 16);   r->drops = get_u16(i + 18);
    r->session = get_u32(i + 20); r->stress = get_u16(i + 24);
}

static void rec_encode(uint8_t *o, const rf_trace_rec_t *r)
{
    switch (r->kind) {
    case RF_TRACE_KIND_TX: rf_trace_tx_encode(o, &r->u.tx); break;
    case RF_TRACE_KIND_RX: rf_trace_rx_encode(o, &r->u.rx); break;
    default:               rf_trace_periodic_encode(o, &r->u.periodic); break;
    }
}

static void rec_decode(const uint8_t *i, uint8_t kind, rf_trace_rec_t *r)
{
    memset(r, 0, sizeof *r);
    r->kind = kind;
    switch (kind) {
    case RF_TRACE_KIND_TX: rf_trace_tx_decode(i, &r->u.tx); break;
    case RF_TRACE_KIND_RX: rf_trace_rx_decode(i, &r->u.rx); break;
    default:               rf_trace_periodic_decode(i, &r->u.periodic); break;
    }
}

/* ── Frame ── */

size_t rf_trace_frame_encode(uint8_t *out, size_t cap, uint8_t kind,
                             const rf_trace_rec_t *rec, size_t count)
{
    size_t rs = rf_trace_rec_size(kind);
    if (out == NULL || rec == NULL || rs == 0) return 0;
    if (count == 0 || count > RF_TRACE_BATCH_MAX) return 0;
    size_t plen = RF_TRACE_PAYLOAD_HDR + count * rs;
    size_t total = RF_TRACE_KR_HDR + plen + 1;
    if (cap < total) return 0;
    for (size_t i = 0; i < count; i++)
        if (rec[i].kind != kind) return 0;

    out[0] = KS_MAGIC_0; out[1] = KR_MAGIC_1; out[2] = RF_TRACE_CMD; out[3] = KS_STATUS_OK;
    put_u16(out + 4, (uint16_t)plen);
    out[6] = RF_TRACE_VERSION; out[7] = kind; out[8] = (uint8_t)count;
    for (size_t i = 0; i < count; i++)
        rec_encode(out + RF_TRACE_HDR + i * rs, &rec[i]);
    out[total - 1] = ks_crc8(out + RF_TRACE_KR_HDR, (uint16_t)plen);
    return total;
}

/* Checks the header as far as `len` bytes go. Returns OK when every byte
 * present is plausible (and sets *total when the 9 header bytes are there,
 * else 0), or the first refusal. Never reads past len. */
static rf_trace_err_t header_check(const uint8_t *b, size_t len, size_t *total)
{
    *total = 0;
    if (len > 0 && b[0] != KS_MAGIC_0)          return RF_TRACE_ERR_MAGIC;
    if (len > 1 && b[1] != KR_MAGIC_1)          return RF_TRACE_ERR_MAGIC;
    if (len > 2 && b[2] != RF_TRACE_CMD)        return RF_TRACE_ERR_CMD;
    if (len > 3 && b[3] != KS_STATUS_OK)        return RF_TRACE_ERR_STATUS;
    if (len > 6 && b[6] != RF_TRACE_VERSION)    return RF_TRACE_ERR_VERSION;
    if (len > 7 && rf_trace_rec_size(b[7]) == 0) return RF_TRACE_ERR_KIND;
    if (len > 8 && (b[8] == 0 || b[8] > RF_TRACE_BATCH_MAX)) return RF_TRACE_ERR_COUNT;
    if (len >= RF_TRACE_HDR) {
        size_t plen = get_u16(b + 4);
        if (plen != RF_TRACE_PAYLOAD_HDR + (size_t)b[8] * rf_trace_rec_size(b[7]))
            return RF_TRACE_ERR_LEN;
        *total = RF_TRACE_KR_HDR + plen + 1;
    } else if (len > 5) {
        /* len known, kind/count not yet: at least bound it. */
        size_t plen = get_u16(b + 4);
        if (plen < RF_TRACE_PAYLOAD_HDR + RF_TRACE_TX_SIZE ||
            plen > RF_TRACE_PAYLOAD_HDR + RF_TRACE_BATCH_MAX * RF_TRACE_REC_SIZE_MAX)
            return RF_TRACE_ERR_LEN;
    }
    return RF_TRACE_OK;
}

/* Full validation of a candidate whose header is complete and whose total
 * length is available; fills out (may be NULL). */
static rf_trace_err_t body_check(const uint8_t *b, size_t total, rf_trace_frame_t *out)
{
    size_t plen = total - RF_TRACE_KR_HDR - 1;
    if (ks_crc8(b + RF_TRACE_KR_HDR, (uint16_t)plen) != b[total - 1]) return RF_TRACE_ERR_CRC;
    if (out != NULL) {
        uint8_t kind = b[7], count = b[8];
        size_t rs = rf_trace_rec_size(kind);
        out->version = b[6]; out->kind = kind; out->count = count;
        for (size_t i = 0; i < count; i++)
            rec_decode(b + RF_TRACE_HDR + i * rs, kind, &out->rec[i]);
    }
    return RF_TRACE_OK;
}

rf_trace_err_t rf_trace_frame_decode(const uint8_t *buf, size_t len, rf_trace_frame_t *out)
{
    if (buf == NULL || out == NULL) return RF_TRACE_ERR_ARG;
    if (len < RF_TRACE_HDR) return RF_TRACE_ERR_SHORT;
    size_t total;
    rf_trace_err_t e = header_check(buf, len, &total);
    if (e != RF_TRACE_OK) return e;
    if (len < total) return RF_TRACE_ERR_SHORT;
    if (len > total) return RF_TRACE_ERR_TRAILING;
    return body_check(buf, total, out);
}

rf_trace_scan_t rf_trace_scan(const uint8_t *buf, size_t len,
                              size_t *start, size_t *flen, rf_trace_frame_t *out)
{
    size_t dummy_s, dummy_l;
    if (start == NULL) start = &dummy_s;
    if (flen == NULL)  flen = &dummy_l;
    *flen = 0;
    if (buf == NULL) { *start = 0; return RF_TRACE_SCAN_NONE; }

    for (size_t i = 0; i < len; i++) {
        if (buf[i] != KS_MAGIC_0) continue;
        size_t avail = len - i, total;
        if (header_check(buf + i, avail, &total) != RF_TRACE_OK)
            continue;                     /* refused: advance by ONE byte */
        if (total == 0 || avail < total) {
            *start = i;                   /* plausible but incomplete */
            return RF_TRACE_SCAN_PARTIAL;
        }
        if (body_check(buf + i, total, out) != RF_TRACE_OK)
            continue;                     /* bad CRC: the real frame may start inside */
        *start = i;
        *flen = total;
        return RF_TRACE_SCAN_FOUND;
    }
    *start = len;
    return RF_TRACE_SCAN_NONE;
}

/* ── Ring ── */

static uint32_t ld(const uint32_t *p)          { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void     st(uint32_t *p, uint32_t v)    { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

bool rf_trace_ring_init(rf_trace_ring_t *r, rf_trace_rec_t *storage, uint32_t cap)
{
    if (r == NULL || storage == NULL || cap == 0 || (cap & (cap - 1)) != 0) return false;
    r->slots = storage; r->cap = cap;
    r->head = r->tail = 0;
    r->dropped = r->drop_seen = 0;
    return true;
}

uint32_t rf_trace_ring_count(const rf_trace_ring_t *r)
{
    return ld(&r->head) - ld(&r->tail);
}

bool rf_trace_ring_push(rf_trace_ring_t *r, const rf_trace_rec_t *rec)
{
    uint32_t h = r->head;                          /* producer owns head */
    if (h - ld(&r->tail) >= r->cap) {
        st(&r->dropped, r->dropped + 1);
        return false;
    }
    r->slots[h & (r->cap - 1)] = *rec;
    st(&r->head, h + 1);                           /* publish after the copy */
    return true;
}

size_t rf_trace_ring_pop_batch(rf_trace_ring_t *r, rf_trace_rec_t *out, size_t max)
{
    uint32_t t = r->tail;                          /* consumer owns tail */
    uint32_t avail = ld(&r->head) - t;
    size_t n = 0;
    if (avail == 0 || max == 0) return 0;
    uint8_t kind = r->slots[t & (r->cap - 1)].kind;
    while (n < max && n < avail) {
        const rf_trace_rec_t *s = &r->slots[(t + (uint32_t)n) & (r->cap - 1)];
        if (s->kind != kind) break;
        out[n++] = *s;
    }
    st(&r->tail, t + (uint32_t)n);
    return n;
}

uint8_t rf_trace_ring_peek_kind(const rf_trace_ring_t *r)
{
    uint32_t t = r->tail;                          /* consumer owns tail */
    if (ld(&r->head) == t) return 0;
    return r->slots[t & (r->cap - 1)].kind;
}

uint32_t rf_trace_ring_drops_take(rf_trace_ring_t *r)
{
    uint32_t d = ld(&r->dropped);
    uint32_t n = d - r->drop_seen;
    r->drop_seen = d;
    return n;
}
