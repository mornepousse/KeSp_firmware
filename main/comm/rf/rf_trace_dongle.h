/* rf-scope, dongle side: RX and periodic trace records, pushed out on the CDC
 * as unsolicited KR frames of KS_CMD_RF_TRACE (0xBA). Format: rf_trace.h;
 * command payload and pure decisions: rf_trace_ctl.h; spec:
 * docs/superpowers/specs/2026-10-20-rf-scope-design.md.
 *
 * Threading — one producer, one consumer, nothing shared but the SPSC ring
 * and two atomics:
 *  - PRODUCER: the rf_rx task, alone. It builds the RX records in its drain
 *    pass, the periodic record (~1 Hz), applies start/stop requests and the
 *    auto-off; it owns every counter, the duplicate state and the deadline.
 *  - CONSUMER: the CDC command task (cdc_acm_com.c), through the pump it
 *    calls after each command batch. Every byte the dongle writes on the CDC
 *    therefore leaves ONE task: a trace frame can never interleave with a
 *    command reply (ks_respond has no lock of its own).
 *  - The CDC handler only POSTS a request (atomic word); the rx task applies
 *    it on its next loop (≤ 1 ms). Off by default, off after every reboot.
 *
 *
 * Back-pressure and the host's presence (review I1/I2, rf_trace_pump_plan):
 *  - a batch is popped only when the CDC TX FIFO has room for its WHOLE
 *    frame on top of the room of the largest reply (KS_PAYLOAD_MAX + 7). A
 *    host that stops reading therefore never gets a truncated frame, and a
 *    command reply never loses its room to the trace: the ring overflows
 *    instead, and the drops are counted in the next periodic record;
 *  - the trace needs DTR (pyserial sets it on open, drops it on close). When
 *    the host closes the port while tracing, the trace STOPS (not just
 *    paused): the ring and the CDC TX FIFO are discarded, so the next open
 *    starts clean instead of receiving minutes-old frames before its first
 *    reply. A host that opens without DTR cannot trace (it stops at once).
 *
 * Cost when off: one relaxed load per received frame and one per rx loop.
 */
#ifndef RF_TRACE_DONGLE_H
#define RF_TRACE_DONGLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rf_trace.h"
#include "rf_trace_ctl.h"

extern bool g_rf_trace_dongle_on;   /* written by the rf_rx task only */

static inline bool rf_trace_dongle_on(void)
{
    return __atomic_load_n(&g_rf_trace_dongle_on, __ATOMIC_RELAXED);
}

/* Ring + CDC pump registration. Called once by rf_rx_start() before the rx
 * task exists; without it (no radio) every request is refused. */
void rf_trace_dongle_init(void);

/* CDC handler side: posts start(duration)/stop. False when the trace is not
 * available (no rx task). The last request posted before the rx task reads
 * it wins. */
bool rf_trace_dongle_request(const rf_trace_ctl_cmd_t *cmd);

/* ── rf_rx task only ──────────────────────────────────────────────────── */

/* Applies a posted request, the auto-off, and emits the periodic record. */
void rf_trace_dongle_service(uint32_t now_ms, uint8_t channel);

/* Fills an RX record for a frame just read (everything but fifo_depth),
 * counts it and its duplicate. */
void rf_trace_dongle_rx_note(rf_trace_rx_t *rec, const uint8_t *buf, uint16_t n,
                             bool mouse_slot, uint8_t pipe, uint8_t rpd,
                             uint8_t channel, bool delivered);

/* Pushes the pass's remaining records (rf_trace_pass_end) and wakes the
 * CDC task. Records are added with rf_trace_pass_add(pass, rec, ring). */
void rf_trace_dongle_pass_end(rf_trace_pass_t *p);
rf_trace_ring_t *rf_trace_dongle_ring(void);

#endif /* RF_TRACE_DONGLE_H */
