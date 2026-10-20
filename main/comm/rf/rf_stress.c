/* rf-scope stress generator — pure part. See rf_stress.h; host-tested in
 * test/test_rf_stress.c. */
#include "rf_stress.h"
#include "rf_packet.h"
#include "cdc_binary_protocol.h"   /* KS_CMD_RF_STRESS */
#include <string.h>

_Static_assert(RF_STRESS_CMD == KS_CMD_RF_STRESS, "one command id");
_Static_assert(RF_STRESS_FRAME_LEN == 2 + RF_HALF_BITMAP_BYTES, "a stress frame is the size of a MATRIX frame");
_Static_assert(RF_STRESS_START_LEN <= 16, "fits the UART0 KS assembler (RF_TRACE_KS_PAYLOAD_MAX)");

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

rf_stress_err_t rf_stress_parse(const uint8_t *p, uint16_t len, rf_stress_cmd_t *out)
{
    if (p == NULL || len == 0 || out == NULL) return RF_STRESS_ERR_LEN;
    memset(out, 0, sizeof *out);
    switch (p[0]) {
    case RF_STRESS_SUB_STOP:
        return len == 1 ? RF_STRESS_OK : RF_STRESS_ERR_LEN;
    case RF_STRESS_SUB_START:
        break;
    default:
        return RF_STRESS_ERR_SUB;
    }
    if (len != RF_STRESS_START_LEN) return RF_STRESS_ERR_LEN;
    rf_stress_params_t *q = &out->p;
    q->pattern        = p[1];
    q->rate_hz        = u16(p + 2);
    q->count          = u32(p + 4);
    q->duration_s     = u16(p + 8);
    q->burst_len      = p[10];
    q->burst_pause_ms = u16(p + 11);
    switch (q->pattern) {
    case RF_STRESS_RATE:
        if (q->rate_hz == 0 || q->rate_hz > RF_STRESS_RATE_MAX_HZ) return RF_STRESS_ERR_RANGE;
        break;
    case RF_STRESS_BURST:
        if (q->burst_len == 0) return RF_STRESS_ERR_RANGE;
        break;
    case RF_STRESS_MAX:
    case RF_STRESS_HELD:
        break;
    default:
        return RF_STRESS_ERR_PATTERN;
    }
    if (q->duration_s == 0)
        q->duration_s = q->count ? (uint16_t)RF_STRESS_COUNT_CAP_S : (uint16_t)RF_STRESS_DEFAULT_S;
    out->start = true;
    return RF_STRESS_OK;
}

size_t rf_stress_frame(uint8_t *out, size_t cap, uint8_t half, uint8_t seq)
{
    if (out == NULL || cap < RF_STRESS_FRAME_LEN) return 0;
    memset(out, 0, RF_STRESS_FRAME_LEN);
    out[0] = (uint8_t)((PKT_TYPE_STRESS << 4) | (half & 0x0F));
    out[1] = seq;
    return RF_STRESS_FRAME_LEN;
}

void rf_stress_init(rf_stress_sched_t *s) { memset(s, 0, sizeof *s); }

void rf_stress_start(rf_stress_sched_t *s, const rf_stress_params_t *p, uint64_t now_us)
{
    uint32_t last = s->last_session + 1u;
    if (last == 0) last = 1;                    /* 0 means "none" in the ack */
    memset(s, 0, sizeof *s);
    s->last_session = s->session = last;
    s->p = *p;
    s->on = true;
    s->start_us = s->next_us = now_us;          /* the first frame at once */
    s->end_us = now_us + (uint64_t)p->duration_s * 1000000u;
}

void rf_stress_stop(rf_stress_sched_t *s, rf_stress_stop_t why)
{
    if (!s->on) return;
    s->on = false;
    s->stopped_by = why;
}

rf_stress_step_t rf_stress_step(rf_stress_sched_t *s, uint64_t now_us, uint64_t *wait_us)
{
    *wait_us = 0;
    if (!s->on) return RF_STRESS_IDLE;
    if (s->p.count && s->sent >= s->p.count) { rf_stress_stop(s, RF_STRESS_STOP_COUNT);    return RF_STRESS_IDLE; }
    if (now_us >= s->end_us)                 { rf_stress_stop(s, RF_STRESS_STOP_DURATION); return RF_STRESS_IDLE; }
    if (now_us >= s->next_us) return RF_STRESS_SEND;
    uint64_t to_next = s->next_us - now_us, to_end = s->end_us - now_us;
    *wait_us = to_next < to_end ? to_next : to_end;   /* both > 0 here */
    return RF_STRESS_WAIT;
}

static uint64_t later(uint64_t a, uint64_t b) { return a > b ? a : b; }

void rf_stress_tx_done(rf_stress_sched_t *s, uint32_t session, uint64_t now_us, rf_stress_tx_t r)
{
    if (!s->on || session != s->session) return;     /* a TX of a stopped / replaced run */
    uint64_t gap = now_us + RF_STRESS_MIN_GAP_US;     /* the earliest next frame: the task blocks */
    if (r == RF_STRESS_TX_UNAVAIL) {                  /* nothing went out: no seq, no count, no burst */
        s->unavail++;
        s->next_us = now_us + RF_STRESS_BACKOFF_US;
        return;
    }
    s->sent++;
    if (r == RF_STRESS_TX_ACK) s->acked++; else s->refused++;
    s->seq++;
    switch (s->p.pattern) {
    case RF_STRESS_RATE:
        /* On the grid of the PREVIOUS due time (no drift from the TX
         * duration); late → rebased after the gap (no catch-up storm). */
        s->next_us = later(s->next_us + 1000000u / s->p.rate_hz, gap);
        break;
    case RF_STRESS_HELD:
        s->next_us = later(s->next_us + (uint64_t)RF_STRESS_HELD_PERIOD_MS * 1000u, gap);
        break;
    case RF_STRESS_BURST:
        if (++s->in_burst >= s->p.burst_len) {
            s->in_burst = 0;
            s->next_us = later(now_us + (uint64_t)s->p.burst_pause_ms * 1000u, gap);
        } else {
            s->next_us = gap;
        }
        break;
    case RF_STRESS_MAX:
    default:
        s->next_us = gap;
        break;
    }
}

uint16_t rf_stress_remaining_s(const rf_stress_sched_t *s, uint64_t now_us)
{
    if (!s->on || now_us >= s->end_us) return 0;
    uint64_t left = (s->end_us - now_us + 999999u) / 1000000u;
    return left > 0xFFFFu ? 0xFFFFu : (uint16_t)left;
}

const char *rf_stress_stop_name(rf_stress_stop_t why)
{
    switch (why) {
    case RF_STRESS_STOP_NONE:     return "-";
    case RF_STRESS_STOP_COUNT:    return "count reached";
    case RF_STRESS_STOP_DURATION: return "duration over";
    case RF_STRESS_STOP_CMD:      return "stop command";
    case RF_STRESS_STOP_SLEEP:    return "sleep";
    }
    return "?";
}
