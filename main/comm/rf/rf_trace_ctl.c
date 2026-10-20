/* rf-scope trace control — pure decisions (rf_trace_ctl.h), host-tested in
 * test/test_rf_trace_ctl.c. */
#include "rf_trace_ctl.h"
#include "rf_trace.h"
#include "rf_packet.h"

rf_trace_ctl_err_t rf_trace_ctl_parse(const uint8_t *p, uint16_t len, rf_trace_ctl_cmd_t *out)
{
    if (p == NULL || len == 0 || out == NULL) return RF_TRACE_CTL_ERR_LEN;
    switch (p[0]) {
    case RF_TRACE_SUB_STOP:
        if (len != 1) return RF_TRACE_CTL_ERR_LEN;
        out->start = false;
        out->duration_s = 0;
        return RF_TRACE_CTL_OK;
    case RF_TRACE_SUB_START: {
        if (len != 1 && len != 3) return RF_TRACE_CTL_ERR_LEN;
        uint16_t d = (len == 3) ? (uint16_t)(p[1] | (p[2] << 8)) : 0;
        out->start = true;
        out->duration_s = d ? d : (uint16_t)RF_TRACE_DEFAULT_S;
        return RF_TRACE_CTL_OK;
    }
    default:
        return RF_TRACE_CTL_ERR_SUB;
    }
}

void rf_trace_timer_start(rf_trace_timer_t *t, uint32_t now_ms, uint16_t duration_s)
{
    t->on = true;
    t->start_ms = now_ms;
    t->dur_ms = (uint32_t)duration_s * 1000u;   /* ≤ 65 535 000 < 2^32 */
}

void rf_trace_timer_stop(rf_trace_timer_t *t)
{
    t->on = false;
}

bool rf_trace_timer_expired(const rf_trace_timer_t *t, uint32_t now_ms)
{
    return t->on && (uint32_t)(now_ms - t->start_ms) >= t->dur_ms;
}

void rf_trace_dup_reset(rf_trace_dup_t *d)
{
    for (unsigned i = 0; i < RF_TRACE_DUP_EMITTERS; i++) d->last[i] = 0;
    d->valid = 0;
}

bool rf_trace_dup_check(rf_trace_dup_t *d, uint8_t emitter, uint8_t seq)
{
    if (emitter >= RF_TRACE_DUP_EMITTERS) return false;
    uint8_t bit = (uint8_t)(1u << emitter);
    bool dup = (d->valid & bit) && d->last[emitter] == seq;
    d->last[emitter] = seq;
    d->valid |= bit;
    return dup;
}

void rf_trace_frame_ident(const uint8_t *buf, uint16_t n, bool mouse_slot,
                          rf_trace_ident_t *out)
{
    out->emitter = mouse_slot ? RF_TRACE_EMIT_MOUSE : RF_TRACE_EMIT_LEFT;
    out->seq = 0;
    out->has_seq = false;
    out->pkt_kind = (buf != NULL && n > 0) ? rf_packet_type(buf, n) : 0;
    if (out->pkt_kind == 0) return;

    uint8_t lo = buf[0] & 0x0F;
    int seq_at = -1;
    switch (out->pkt_kind) {
    case PKT_TYPE_MATRIX:
        if (!mouse_slot) out->emitter = (lo == RF_HALF_RIGHT) ? RF_TRACE_EMIT_RIGHT : RF_TRACE_EMIT_LEFT;
        seq_at = 1 + RF_HALF_BITMAP_BYTES;
        break;
    case PKT_TYPE_STRESS:
        if (!mouse_slot) out->emitter = (lo == RF_HALF_RIGHT) ? RF_TRACE_EMIT_RIGHT : RF_TRACE_EMIT_LEFT;
        seq_at = 1;
        break;
    case PKT_TYPE_STATUS:
        if (!mouse_slot && (lo & PKT_STATUS_FLAG_HALF_RIGHT)) out->emitter = RF_TRACE_EMIT_RIGHT;
        seq_at = 3;
        break;
    case PKT_TYPE_HEARTBEAT:
        seq_at = 3 + RF_HALF_BITMAP_BYTES;
        break;
    default:
        break;   /* HIDREPORT, SYNC_REQ, …: no sequence number */
    }
    if (seq_at >= 0 && n > (uint16_t)seq_at) {
        out->seq = buf[seq_at];
        out->has_seq = true;
    }
}

uint8_t rf_trace_fifo_depth(size_t pass_count, size_t idx)
{
    if (idx >= pass_count) return 0;
    size_t d = pass_count - idx;
    return (uint8_t)(d > 3 ? 3 : d);
}

static void pass_push(const rf_trace_rx_t *r, uint8_t depth, rf_trace_ring_t *ring)
{
    rf_trace_rec_t rec = { .kind = RF_TRACE_KIND_RX };
    rec.u.rx = *r;
    rec.u.rx.fifo_depth = depth;
    rf_trace_ring_push(ring, &rec);
}

void rf_trace_pass_add(rf_trace_pass_t *p, const rf_trace_rx_t *rec, rf_trace_ring_t *ring)
{
    if (p->n == RF_TRACE_PASS_MAX) {
        /* Every record but the last two has ≥ 2 successors: depth 3, final. */
        size_t keep = 2, out = p->n - keep;
        for (size_t i = 0; i < out; i++) pass_push(&p->rec[i], 3, ring);
        for (size_t i = 0; i < keep; i++) p->rec[i] = p->rec[out + i];
        p->n = keep;
    }
    p->rec[p->n++] = *rec;
}

size_t rf_trace_pass_end(rf_trace_pass_t *p, rf_trace_ring_t *ring)
{
    size_t n = p->n;
    for (size_t i = 0; i < n; i++) pass_push(&p->rec[i], rf_trace_fifo_depth(n, i), ring);
    p->n = 0;
    return n;
}

bool rf_trace_drop_before_engine(const uint8_t *buf, uint16_t n)
{
    return buf != NULL && n > 0 && rf_packet_type(buf, n) == PKT_TYPE_STRESS;
}
