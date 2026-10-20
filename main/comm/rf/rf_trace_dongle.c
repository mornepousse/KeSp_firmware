/* rf-scope, dongle side — see rf_trace_dongle.h for the threading contract.
 * The decisions (command, deadline, duplicates, depth, stress drop) are pure
 * and host-tested in rf_trace_ctl.c; this file only wires them to the rf_rx
 * task, the esp_timer clock and the CDC. */
#include "rf_trace_dongle.h"
#include "cdc_binary_protocol.h"   /* ks_respond, KS_CMD_RF_TRACE */
#include "cdc_acm_com.h"           /* cdc_set_unsolicited_pump, cdc_wake */
#include "rf_packet.h"             /* PKT_TYPE_STRESS */
#include "esp_timer.h"
#include "sdkconfig.h"

/* ── Shared between the tasks (atomics + the SPSC ring) ── */
bool g_rf_trace_dongle_on;                 /* written by the rf_rx task only */
#define REQ_NONE  (-1)
static int32_t s_req = REQ_NONE;           /* CDC → rx task: 0 stop, > 0 start(duration_s) */
static uint32_t s_req_session;             /* CDC → rx task: the start's session, stored BEFORE s_req */
static uint32_t s_session_next;            /* CDC task only: the numbering of the starts */
static uint32_t s_session;                 /* rx task writes (atomic): the running session, 0 = off */
static bool    s_ready;                    /* init done: the rx task exists */

#define RING_CAP 128u                      /* ~3.5 KB; 8 frames of 16 records */
static rf_trace_rec_t  s_slots[RING_CAP];
static rf_trace_ring_t s_ring;

/* ── rf_rx task only ── */
static rf_trace_timer_t s_timer;
static rf_trace_dup_t   s_dup;
static uint32_t s_last_periodic_ms;
static uint16_t s_frames, s_dups, s_stress; /* since the previous periodic record */

static uint32_t now_us32(void) { return (uint32_t)esp_timer_get_time(); }

rf_trace_ring_t *rf_trace_dongle_ring(void) { return &s_ring; }

/* ── Consumer: the CDC command task ── */

/* The trace never takes the room of the largest reply: a command answered
 * while the host is slow still finds room for its whole frame. */
#define CDC_REPLY_RESERVE ((uint32_t)KS_PAYLOAD_MAX + 7u)
#if defined(CONFIG_TINYUSB_CDC_TX_BUFSIZE)
_Static_assert(CONFIG_TINYUSB_CDC_TX_BUFSIZE >= CDC_REPLY_RESERVE + RF_TRACE_FRAME_MAX,
               "CDC TX FIFO too small for a reply AND a trace frame: the trace would never be sent");
#endif

static void discard_ring(rf_trace_rec_t *scratch)
{
    while (rf_trace_ring_pop_batch(&s_ring, scratch, RF_TRACE_BATCH_MAX) != 0)
        ;
}

static void pump(void)
{
    static rf_trace_rec_t batch[RF_TRACE_BATCH_MAX];
    static uint8_t frame[RF_TRACE_FRAME_MAX];
    /* Bounded per call so a command waiting behind a burst is not starved;
     * whatever remains re-arms the task. */
    for (unsigned i = 0; i < RING_CAP / RF_TRACE_BATCH_MAX; i++) {
        size_t n;
        switch (rf_trace_pump_plan(cdc_host_connected(), rf_trace_dongle_on(),
                                   rf_trace_ring_peek_kind(&s_ring), cdc_tx_room(),
                                   CDC_REPLY_RESERVE, &n)) {
        case RF_TRACE_PUMP_IDLE:
            return;
        case RF_TRACE_PUMP_HOLD:
            /* The host stopped reading: pop nothing, do not re-wake. The
             * ring overflows and counts its drops (the next periodic record
             * reports them); the next producer wake or the CDC task's 1 s
             * safety net tries again. */
            return;
        case RF_TRACE_PUMP_STOP: {
            /* The host closed the port (DTR dropped) while tracing: stop the
             * trace, and throw away what nobody will read — the ring and the
             * TX FIFO — so the next open does not start with stale frames. */
            const rf_trace_ctl_cmd_t stop = { .start = false };
            uint32_t sess; uint16_t rem;
            (void)rf_trace_dongle_request(&stop, &sess, &rem);
            cdc_tx_discard();
            discard_ring(batch);
            return;
        }
        case RF_TRACE_PUMP_DISCARD:
            discard_ring(batch);               /* the stop's final record, no host */
            return;
        case RF_TRACE_PUMP_SEND:
            break;
        }
        n = rf_trace_ring_pop_batch(&s_ring, batch, n);
        if (n == 0) return;
        size_t len = rf_trace_frame_encode(frame, sizeof frame, batch[0].kind, batch, n);
        if (len > RF_TRACE_KR_HDR + 1u)
            ks_respond(KS_CMD_RF_TRACE, KS_STATUS_OK, frame + RF_TRACE_KR_HDR,
                       (uint16_t)(len - RF_TRACE_KR_HDR - 1u));   /* same bytes, same CRC; room checked */
    }
    if (rf_trace_ring_count(&s_ring) != 0) cdc_wake();
}

void rf_trace_dongle_init(void)
{
    rf_trace_ring_init(&s_ring, s_slots, RING_CAP);
    cdc_set_unsolicited_pump(pump);
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
}

bool rf_trace_dongle_request(const rf_trace_ctl_cmd_t *cmd, uint32_t *session, uint16_t *remaining_s)
{
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) return false;
    if (cmd->start) {
        uint32_t s = ++s_session_next;
        if (s == 0) s = ++s_session_next;              /* 0 means off */
        __atomic_store_n(&s_req_session, s, __ATOMIC_RELEASE);
        *session = s;
        *remaining_s = cmd->duration_s;
    } else {
        *session = __atomic_load_n(&s_session, __ATOMIC_ACQUIRE);   /* the one being stopped */
        *remaining_s = 0;
    }
    __atomic_store_n(&s_req, cmd->start ? (int32_t)cmd->duration_s : 0, __ATOMIC_RELEASE);
    return true;
}

/* ── Producer: the rf_rx task ── */

static void push_periodic(uint32_t now_ms, uint8_t channel)
{
    rf_trace_rec_t rec = { .kind = RF_TRACE_KIND_PERIODIC };
    rec.u.periodic = (rf_trace_periodic_t){
        .t_us = now_us32(),
        .uptime_s = now_ms / 1000u,
        .source = RF_TRACE_EMIT_DONGLE,
        .state = RF_TRACE_ST_AWAKE,        /* the dongle never sleeps */
        .batt_dV = 0,                      /* USB powered */
        .channel = channel,
        .frames = s_frames,
        .fails = 0,
        .dups = s_dups,
        /* drops_take is called from this task only (both drop counters are
         * then the producer's — the consumer never reads them). */
        .drops = (uint16_t)rf_trace_ring_drops_take(&s_ring),
        .session = __atomic_load_n(&s_session, __ATOMIC_RELAXED),
        .stress = s_stress,                /* stress frames received and DROPPED */
    };
    rf_trace_ring_push(&s_ring, &rec);
    s_frames = s_dups = s_stress = 0;
    s_last_periodic_ms = now_ms;
}

void rf_trace_dongle_service(uint32_t now_ms, uint8_t channel)
{
    bool on = g_rf_trace_dongle_on;
    int32_t req = REQ_NONE;
    if (__atomic_load_n(&s_req, __ATOMIC_RELAXED) != REQ_NONE)
        req = __atomic_exchange_n(&s_req, REQ_NONE, __ATOMIC_ACQ_REL);
    if (!on && req == REQ_NONE) return;          /* the off fast path */

    bool wake = false;
    if (req > 0) {                                /* start, or restart */
        if (on) push_periodic(now_ms, channel);   /* close the previous run's counters */
        rf_trace_timer_start(&s_timer, now_ms, (uint16_t)req);
        rf_trace_dup_reset(&s_dup);
        s_frames = s_dups = s_stress = 0;
        (void)rf_trace_ring_drops_take(&s_ring);  /* drops of a previous run are not this one's */
        __atomic_store_n(&s_session, __atomic_load_n(&s_req_session, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        __atomic_store_n(&g_rf_trace_dongle_on, true, __ATOMIC_RELAXED);
        push_periodic(now_ms, channel);           /* the run's first record */
        wake = true;
    } else if (on && (req == 0 || rf_trace_timer_expired(&s_timer, now_ms))) {
        push_periodic(now_ms, channel);           /* final counters, still under the session */
        rf_trace_timer_stop(&s_timer);
        __atomic_store_n(&s_session, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_rf_trace_dongle_on, false, __ATOMIC_RELAXED);
        wake = true;
    } else if (on && (uint32_t)(now_ms - s_last_periodic_ms) >= RF_TRACE_PERIODIC_MS) {
        push_periodic(now_ms, channel);
        wake = true;
    }
    if (wake) cdc_wake();
}

void rf_trace_dongle_rx_note(rf_trace_rx_t *rec, const uint8_t *buf, uint16_t n,
                             bool mouse_slot, uint8_t pipe, uint8_t rpd,
                             uint8_t channel, bool delivered)
{
    rf_trace_ident_t id;
    rf_trace_frame_ident(buf, n, mouse_slot, &id);
    bool dup = id.has_seq && rf_trace_dup_check(&s_dup, id.emitter, id.seq);
    *rec = (rf_trace_rx_t){
        .t_us = now_us32(),
        .emitter = id.emitter,
        .seq = id.seq,
        .pkt_kind = id.pkt_kind,
        .payload_len = (uint8_t)n,
        .pipe = pipe,
        .rpd = rpd,
        .channel = channel,
        .fifo_depth = 0,                          /* fixed at the end of the pass */
        .flags = rf_trace_rx_flags(delivered, dup, id.has_seq),
    };
    s_frames++;
    if (id.pkt_kind == PKT_TYPE_STRESS) s_stress++;
    if (dup) s_dups++;
}

void rf_trace_dongle_pass_end(rf_trace_pass_t *p)
{
    if (rf_trace_pass_end(p, &s_ring) != 0) cdc_wake();
}
