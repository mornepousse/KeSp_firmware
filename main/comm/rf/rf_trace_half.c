/* rf-scope, half side — pure decisions. See rf_trace_half.h. */
#include "rf_trace_half.h"
#include "cdc_binary_protocol.h"   /* ks_crc8, KS_MAGIC_0, KS_MAGIC_1, KR_MAGIC_1 */
#include <string.h>

uint8_t rf_trace_tx_result(bool ok, uint8_t status)
{
    if (ok) return RF_TRACE_TX_ACK;
    return (status & 0x10) ? RF_TRACE_TX_MAX_RT : RF_TRACE_TX_TIMEOUT;
}

/* ── OBSERVE_TX ── */

uint8_t rf_trace_observe_step(rf_trace_plos_t *st, uint8_t observe_tx, uint32_t session,
                              rf_plos_reset_t reset, uint8_t *arc)
{
    uint8_t cur = (uint8_t)(observe_tx >> 4), delta;
    *arc = (uint8_t)(observe_tx & 0x0F);
    if (reset == RF_PLOS_RESET_NOW)        delta = cur;
    else if (st->session != session)       delta = 0;      /* baseline of a new session */
    else if (reset == RF_PLOS_RESET_SINCE) delta = cur;
    else                                   delta = (cur >= st->prev) ? (uint8_t)(cur - st->prev) : cur;
    st->prev = cur; st->session = session;
    return delta;
}

/* ── Session control ── */

void rf_trace_sess_init(rf_trace_sess_t *c) { memset(c, 0, sizeof *c); }

void rf_trace_sess_start(rf_trace_sess_t *c, uint32_t now_ms, uint16_t duration_s)
{
    uint32_t s = c->last_session + 1u;
    if (s == 0) s = 1;                             /* 0 means off */
    c->last_session = c->session = s;
    rf_trace_timer_start(&c->timer, now_ms, duration_s ? duration_s : RF_TRACE_DEFAULT_S);
    c->next_periodic_ms = now_ms;                  /* first record at once: uptime for the host */
    c->asleep = false;
}

void rf_trace_sess_stop(rf_trace_sess_t *c) { c->session = 0; c->asleep = false; rf_trace_timer_stop(&c->timer); }

static bool due(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

rf_trace_act_t rf_trace_sess_tick(rf_trace_sess_t *c, uint32_t now_ms, uint32_t *wait_ms)
{
    *wait_ms = UINT32_MAX;
    if (c->session == 0) return RF_TRACE_ACT_NONE;
    if (rf_trace_timer_expired(&c->timer, now_ms)) {
        rf_trace_sess_stop(c);
        return RF_TRACE_ACT_EXPIRED;
    }
    rf_trace_act_t a = RF_TRACE_ACT_NONE;
    if (!c->asleep && due(now_ms, c->next_periodic_ms)) {
        a = RF_TRACE_ACT_PERIODIC;
        /* On the grid; after a long gap (a busy CPU) restart from now. */
        c->next_periodic_ms += RF_TRACE_PERIODIC_MS;
        if (due(now_ms, c->next_periodic_ms)) c->next_periodic_ms = now_ms + RF_TRACE_PERIODIC_MS;
    }
    uint32_t to_end  = c->timer.dur_ms - (uint32_t)(now_ms - c->timer.start_ms);
    uint32_t to_next = due(now_ms, c->next_periodic_ms) ? RF_TRACE_PERIODIC_MS
                                                        : (uint32_t)(c->next_periodic_ms - now_ms);
    *wait_ms = to_next < to_end ? to_next : to_end;
    return a;
}

bool rf_trace_sess_sleep(rf_trace_sess_t *c)
{
    if (c->session == 0 || c->asleep) return false;
    c->asleep = true;
    return true;
}

bool rf_trace_sess_wake(rf_trace_sess_t *c, uint32_t now_ms)
{
    if (c->session == 0 || !c->asleep) return false;
    c->asleep = false;
    if (rf_trace_timer_expired(&c->timer, now_ms)) { rf_trace_sess_stop(c); return false; }
    c->next_periodic_ms = now_ms + RF_TRACE_PERIODIC_MS;   /* the WOKE record stands for this period */
    return true;
}

/* ── KS request assembler ── */

enum { KS_IDLE = 0, KS_GOT_K, KS_CMD, KS_LEN_LO, KS_LEN_HI, KS_PAYLOAD, KS_CRC };

void rf_trace_ks_rx_reset(rf_trace_ks_rx_t *rx) { memset(rx, 0, sizeof *rx); }

bool rf_trace_ks_rx_feed(rf_trace_ks_rx_t *rx, uint8_t b)
{
    switch (rx->st) {
    case KS_IDLE:
        if (b == KS_MAGIC_0) rx->st = KS_GOT_K;
        return false;
    case KS_GOT_K:
        rx->st = (b == KS_MAGIC_1) ? KS_CMD : (b == KS_MAGIC_0) ? KS_GOT_K : KS_IDLE;
        return false;
    case KS_CMD:
        rx->cmd = b; rx->st = KS_LEN_LO;
        return false;
    case KS_LEN_LO:
        rx->len = b; rx->st = KS_LEN_HI;
        return false;
    case KS_LEN_HI:
        rx->len |= (uint16_t)(b << 8);
        rx->got = 0;
        if (rx->len > RF_TRACE_KS_PAYLOAD_MAX) { rx->st = KS_IDLE; return false; }   /* refused now, not after 4 KB */
        rx->st = rx->len ? KS_PAYLOAD : KS_CRC;
        return false;
    case KS_PAYLOAD:
        rx->payload[rx->got++] = b;
        if (rx->got >= rx->len) rx->st = KS_CRC;
        return false;
    case KS_CRC:
    default:
        rx->st = KS_IDLE;
        return b == ks_crc8(rx->payload, rx->len);
    }
}

size_t rf_trace_kr_encode(uint8_t *out, size_t cap, uint8_t cmd, uint8_t status,
                          const uint8_t *payload, uint16_t len)
{
    size_t total = 7u + len;
    if (out == NULL || cap < total || (len && payload == NULL)) return 0;
    out[0] = KS_MAGIC_0; out[1] = KR_MAGIC_1; out[2] = cmd; out[3] = status;
    out[4] = (uint8_t)len; out[5] = (uint8_t)(len >> 8);
    if (len) memcpy(out + 6, payload, len);
    out[6 + len] = ks_crc8(payload, len);
    return total;
}
