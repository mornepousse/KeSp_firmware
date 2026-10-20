/* rf-scope trace control — the pure decisions around the trace records
 * (rf_trace.h): the KS_CMD_RF_TRACE (0xBA) request payload, the auto-off
 * deadline, the per-emitter duplicate detector, the identity of a received
 * radio frame, the FIFO depth estimate, and the rule that drops a synthetic
 * stress frame before the fusion engine.
 *
 * PURE: no ESP-IDF include, host-tested (test/test_rf_trace_ctl.c). Used by
 * the dongle (rf_trace_dongle.c); the command parser and the deadline are
 * written to be reused by the halves (UART0 RF_TRACE start/stop).
 *
 * ── Request payload (KS frame, cmd 0xBA) ─────────────────────────────────
 *   stop : [0x00]
 *   start: [0x01]                       → default duration (600 s)
 *          [0x01][duration_s:u16 LE]    → 0 = default duration
 * Any other length or sub-command is refused (ERR_INVALID on the wire). A
 * start while the trace is on RESTARTS it with the new duration (counters,
 * duplicate state and deadline reset); a stop while off is a no-op success.
 * The reply is a KR of cmd 0xBA carrying the 7-byte ack below (errors: status
 * only, no payload): a trace data frame always has a payload ≥ 16 bytes, so
 * the host tells them apart by length.
 *
 * ── Ack payload (RF_TRACE and RF_STRESS start/stop) ──────────────────────
 *   [sub][session:u32 LE][remaining_s:u16 LE]          RF_TRACE_ACK_LEN = 7
 *   start: the new session's number and its duration;
 *   stop : the session that was stopped (0 if none was on) and 0.
 * The host learns which session the following records belong to (periodic
 * `session`) and how long the device will go on on its own.
 */
#ifndef RF_TRACE_CTL_H
#define RF_TRACE_CTL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rf_trace.h"

#define RF_TRACE_SUB_STOP        0x00u
#define RF_TRACE_SUB_START       0x01u
#define RF_TRACE_DEFAULT_S       600u     /* 10 min, spec decision 3 */
#define RF_TRACE_PERIODIC_MS     1000u    /* periodic record cadence */

typedef enum {
    RF_TRACE_CTL_OK = 0,
    RF_TRACE_CTL_ERR_LEN,     /* wrong length for the sub-command (or NULL payload) */
    RF_TRACE_CTL_ERR_SUB,     /* unknown sub-command */
} rf_trace_ctl_err_t;

typedef struct {
    bool     start;           /* false = stop */
    uint16_t duration_s;      /* start only, never 0 after parse (default applied) */
} rf_trace_ctl_cmd_t;

rf_trace_ctl_err_t rf_trace_ctl_parse(const uint8_t *p, uint16_t len, rf_trace_ctl_cmd_t *out);

/* ── Auto-off deadline: elapsed-time comparison, safe across the u32 ms wrap
 * (49.7 days) — never compares two absolute times. */
typedef struct {
    bool     on;
    uint32_t start_ms;
    uint32_t dur_ms;
} rf_trace_timer_t;

void rf_trace_timer_start(rf_trace_timer_t *t, uint32_t now_ms, uint16_t duration_s);
void rf_trace_timer_stop(rf_trace_timer_t *t);
/* True once `duration` has elapsed since start (false when off). */
bool rf_trace_timer_expired(const rf_trace_timer_t *t, uint32_t now_ms);
/* Whole seconds left, rounded UP (0 when off or over), for the ack. */
uint16_t rf_trace_timer_remaining_s(const rf_trace_timer_t *t, uint32_t now_ms);

#define RF_TRACE_ACK_LEN 7u
/* Writes the ack payload; returns RF_TRACE_ACK_LEN, 0 when out is NULL or
 * cap is too small. */
size_t rf_trace_ack_encode(uint8_t *out, size_t cap, uint8_t sub, uint32_t session, uint16_t remaining_s);

/* ── Duplicate detector: a frame is a duplicate when it carries the same seq
 * as the PREVIOUS frame of the same emitter (an application-level re-send
 * whose first copy got through; the nRF24 already discards ESB re-sends by
 * PID). Emitters 0..3 (RF_TRACE_EMIT_*); out of range is never a duplicate. */
#define RF_TRACE_DUP_EMITTERS 4u
typedef struct {
    uint8_t last[RF_TRACE_DUP_EMITTERS];
    uint8_t valid;            /* bit e = last[e] holds a seq */
} rf_trace_dup_t;

void rf_trace_dup_reset(rf_trace_dup_t *d);
/* Returns true when seq equals the previous seq of that emitter; records seq. */
bool rf_trace_dup_check(rf_trace_dup_t *d, uint8_t emitter, uint8_t seq);

/* ── Identity of a received radio frame ──────────────────────────────────
 * emitter: mouse slot → MOUSE; keyboard slot → the half named by the frame
 * (MATRIX and STRESS low nibble, STATUS flag), LEFT for every other kind
 * (SYNC_REQ and the legacy heartbeat only come from the left, HIDREPORT
 * from the keyboard that runs its engine — the left).
 * seq: MATRIX, STATUS, HEARTBEAT and STRESS carry one; HIDREPORT and
 * SYNC_REQ do not (has_seq false, seq 0, never a duplicate). */
typedef struct {
    uint8_t emitter;
    uint8_t seq;
    uint8_t pkt_kind;         /* high nibble of byte 0 (PKT_TYPE_*), 0 if empty */
    bool    has_seq;
} rf_trace_ident_t;

void rf_trace_frame_ident(const uint8_t *buf, uint16_t n, bool mouse_slot,
                          rf_trace_ident_t *out);

/* FIFO depth when the idx-th frame (0-based) of a drain pass that read
 * `pass_count` frames was read: the frames still queued, itself included,
 * capped at the nRF24's 3 levels. The chip does not expose a count, so it is
 * reconstructed from the pass (a frame that arrived during the pass counts
 * too). 3 = FIFO full: a frame arriving then is not acknowledged. */
uint8_t rf_trace_fifo_depth(size_t pass_count, size_t idx);

/* A drain pass in progress: RX records wait here until the pass ends, so
 * their FIFO depth can be reconstructed. A full pass pushes the records
 * whose depth is already certain (3 — at least two more frames follow) and
 * keeps the last two. Pushes go to the ring (drops counted there). */
#define RF_TRACE_PASS_MAX 8u
typedef struct {
    rf_trace_rx_t rec[RF_TRACE_PASS_MAX];
    size_t n;
} rf_trace_pass_t;

void rf_trace_pass_add(rf_trace_pass_t *p, const rf_trace_rx_t *rec, rf_trace_ring_t *ring);
/* End of the pass: depths fixed, every record pushed, the pass emptied.
 * Returns the number of records handed to the ring (pushed or dropped). */
size_t rf_trace_pass_end(rf_trace_pass_t *p, rf_trace_ring_t *ring);

/* ── Stress frames never reach the fusion engine ─────────────────────────
 * True for a frame the dongle must DROP before any handler (engine, HID,
 * battery cache, link supervision): the synthetic PKT_TYPE_STRESS of the
 * rf-scope stress generator (Task 5b). Applied whether tracing is on or
 * off. A stress frame is not proof of life either: it says nothing about
 * the keyboard's state. */
bool rf_trace_drop_before_engine(const uint8_t *buf, uint16_t n);

/* RX record flags. A frame without a seq is never a duplicate. */
uint8_t rf_trace_rx_flags(bool delivered, bool dup, bool has_seq);

/* ── The dongle's dispatch decision for one received frame ───────────────
 * drain_radio (rf_rx_task.c) switches on this and on nothing else, so the
 * host test of this function is the proof that a stress frame reaches no
 * handler (test_rf_rx_dispatch_never_hands_a_stress_frame_to_a_handler):
 *   STRESS_DROP — synthetic: traced if tracing, then dropped. No engine, no
 *                 HID, no battery cache, no proof of life, no ACK payload;
 *   STATUS      — battery cache, link quality, left's mode and fingerprint;
 *   HEARTBEAT   — legacy: link quality, battery;
 *   HIDREPORT   — straight to the host (mouse; pre-fusion keyboard);
 *   MATRIX      — the fusion engine (and the right's re-emission);
 *   OTHER       — no handler (SYNC_REQ only feeds the sync ACK payload).
 * `trace` = tracing, for every frame (a dropped one included). */
typedef enum {
    RF_RX_ROUTE_OTHER = 0,
    RF_RX_ROUTE_STRESS_DROP,
    RF_RX_ROUTE_STATUS,
    RF_RX_ROUTE_HEARTBEAT,
    RF_RX_ROUTE_HIDREPORT,
    RF_RX_ROUTE_MATRIX,
} rf_rx_route_t;

typedef struct {
    rf_rx_route_t route;
    bool proof_of_life;      /* rf_slot_link_rx: the keyboard is alive */
    bool ack_payload_ok;     /* may be followed by the sync ACK payload */
    bool trace;              /* an RX record is written */
} rf_rx_decision_t;

rf_rx_decision_t rf_rx_dispatch(const uint8_t *buf, uint16_t n, bool tracing);
/* MATRIX (engine) and HIDREPORT (host): what ends up typed. */
bool rf_rx_route_reaches_engine(rf_rx_route_t r);
/* Any route drain_radio has code for (everything but OTHER and STRESS_DROP). */
bool rf_rx_route_has_handler(rf_rx_route_t r);

/* ── Output budget: a frame is written WHOLE or not at all ───────────────
 * Number of records of `kind` whose encoded frame (header + k records + CRC)
 * fits in `room` bytes, capped at RF_TRACE_BATCH_MAX; 0 when not even one
 * fits or the kind is unknown. Both ends size their batch with it BEFORE
 * popping: the half to the UART0 TX FIFO's free bytes (128-byte FIFO: 9
 * TX records, 4 periodic), the dongle to the CDC TX FIFO's. */
size_t rf_trace_batch_fit(uint32_t room, uint8_t kind);

/* The dongle's CDC pump, one decision per batch (rf_trace_dongle.c):
 *   STOP    — the host is gone (no DTR) while tracing: stop the trace and
 *             discard the ring and the TX FIFO (nothing stale on reopen);
 *   DISCARD — host gone, trace off, records left: drop them, never send;
 *   IDLE    — nothing to send;
 *   HOLD    — the TX FIFO lacks room for one whole frame beyond `reserve`
 *             (the largest reply's room): pop nothing, do not re-wake — the
 *             ring overflows and counts its drops;
 *   SEND    — pop *n records (≥ 1, ≤ BATCH_MAX) of next_kind and send them.
 * next_kind = rf_trace_ring_peek_kind() (0 = empty ring). *n is 0 except
 * for SEND. */
typedef enum {
    RF_TRACE_PUMP_IDLE = 0,
    RF_TRACE_PUMP_SEND,
    RF_TRACE_PUMP_HOLD,
    RF_TRACE_PUMP_DISCARD,
    RF_TRACE_PUMP_STOP,
} rf_trace_pump_t;
rf_trace_pump_t rf_trace_pump_plan(bool connected, bool tracing, uint8_t next_kind,
                                   uint32_t tx_free, uint32_t reserve, size_t *n);

#endif /* RF_TRACE_CTL_H */
