/* rf-scope, half side — the PURE decisions (host-tested, test/test_rf_trace_half.c).
 *
 * The hardware glue (ring, drain task, UART0 bytes, sleep hook) lives in
 * rf_trace_uart.c; the TX records are built by the radio owner
 * (radio_owner.c), the only module that sees every transmission. Spec:
 * docs/superpowers/specs/2026-10-20-rf-scope-design.md. Wire format:
 * rf_trace.h. No ESP-IDF include here. */
#ifndef RF_TRACE_HALF_H
#define RF_TRACE_HALF_H

#include "rf_trace.h"
#include "rf_trace_ctl.h"   /* Task 2: request parser, auto-off timer, frame identity — one semantics on both ends */

/* RF_TRACE_TX_ACK when the driver said ok, else MAX_RT if the STATUS read at
 * the end of the poll had bit4 (MAX_RT), else TIMEOUT (neither flag came up
 * within the poll budget — the payload was flushed). */
uint8_t rf_trace_tx_result(bool ok, uint8_t status);

/* ── OBSERVE_TX (nRF24 register 0x08): [7:4] PLOS_CNT, [3:0] ARC_CNT ───────
 * ARC_CNT = retransmissions of the last packet. PLOS_CNT = packets lost
 * (MAX_RT) since the last write of RF_CH, saturating at 15. The record wants
 * the PLOS increase since the previous traced packet:
 *   RESET_NOW   — RF_CH was written right before THIS packet (an excursion
 *                 sets the far channel): delta = cur;
 *   a new trace session (st->session != session) — the previous reading is
 *                 from another session or none: delta = 0, a baseline;
 *   RESET_SINCE — RF_CH was written (mode change, re-arm, pairing round)
 *                 since the previous traced packet: delta = cur;
 *   otherwise   — cur - prev, or cur when cur < prev (a reset nobody told us
 *                 about). A saturated counter (15 → 15) reads 0: the record's
 *                 `result` still says MAX_RT. */
typedef enum { RF_PLOS_NO_RESET = 0, RF_PLOS_RESET_SINCE, RF_PLOS_RESET_NOW } rf_plos_reset_t;
typedef struct { uint8_t prev; uint32_t session; } rf_trace_plos_t;
uint8_t rf_trace_observe_step(rf_trace_plos_t *st, uint8_t observe_tx, uint32_t session,
                              rf_plos_reset_t reset, uint8_t *arc);

/* ── Session control: start/stop, auto-off, periodic cadence, sleep ───────
 * One instance, owned by rf_trace_uart.c under its lock. session = 0 means
 * off; every start takes a new non-zero number (the radio owner compares it
 * to know a PLOS reading belongs to the current session). Times are
 * esp_timer milliseconds truncated to 32 bits: every comparison is
 * wrap-safe. While asleep nothing is emitted: the record before the sleep
 * says SLEEPING, the first one after it says WOKE. */
typedef struct {
    uint32_t session;          /* 0 = off */
    uint32_t last_session;     /* numbering, survives stop */
    rf_trace_timer_t timer;    /* auto-off (rf_trace_ctl.h) */
    uint32_t next_periodic_ms; /* RF_TRACE_PERIODIC_MS grid (rf_trace_ctl.h) */
    bool     asleep;
} rf_trace_sess_t;

typedef enum {
    RF_TRACE_ACT_NONE = 0,
    RF_TRACE_ACT_PERIODIC,     /* emit a periodic record, state AWAKE */
    RF_TRACE_ACT_EXPIRED,      /* the duration ran out: tracing is now off */
} rf_trace_act_t;

void rf_trace_sess_init(rf_trace_sess_t *c);
/* duration_s = 0 → RF_TRACE_DEFAULT_S. A start while on restarts the clock
 * (new session). The first periodic record is due at once (it carries the
 * uptime the host needs to unwrap t_us). */
void rf_trace_sess_start(rf_trace_sess_t *c, uint32_t now_ms, uint16_t duration_s);
void rf_trace_sess_stop(rf_trace_sess_t *c);
/* *wait_ms = how long the caller may block before the next due event
 * (UINT32_MAX when off). Never 0 while on: a due event is consumed here. */
rf_trace_act_t rf_trace_sess_tick(rf_trace_sess_t *c, uint32_t now_ms, uint32_t *wait_ms);
/* Sleep hook: true → emit ONE record with state SLEEPING (on and awake only;
 * deep sleep calls the hooks a second time after light sleep). */
bool rf_trace_sess_sleep(rf_trace_sess_t *c);
/* Wake hook: true → emit ONE record with state WOKE. A duration that ran
 * out during the sleep turns tracing off here instead (false). */
bool rf_trace_sess_wake(rf_trace_sess_t *c, uint32_t now_ms);

/* ── UART0 RX: KS request assembler (no ASCII parser) ─────────────────────
 * Same framing as the CDC: [K][S][cmd][len u16 LE][payload][crc8(payload)].
 * Fed byte by byte; true when a complete request with a valid CRC ends on
 * this byte: rx->cmd, rx->payload[0..rx->len). Any command is assembled
 * (RF_STRESS will ride the same reader); a payload above
 * RF_TRACE_KS_PAYLOAD_MAX or a bad CRC is dropped and the assembler
 * resynchronises — a 'K' that breaks a header starts the next one. */
#define RF_TRACE_KS_PAYLOAD_MAX 16u
typedef struct {
    uint8_t  st;
    uint8_t  cmd;
    uint16_t len, got;
    uint8_t  payload[RF_TRACE_KS_PAYLOAD_MAX];
} rf_trace_ks_rx_t;
void rf_trace_ks_rx_reset(rf_trace_ks_rx_t *rx);
bool rf_trace_ks_rx_feed(rf_trace_ks_rx_t *rx, uint8_t b);

/* A KR reply, byte for byte what ks_respond() writes. Returns its length,
 * 0 when cap is too small. */
size_t rf_trace_kr_encode(uint8_t *out, size_t cap, uint8_t cmd, uint8_t status,
                          const uint8_t *payload, uint16_t len);

#endif /* RF_TRACE_HALF_H */
