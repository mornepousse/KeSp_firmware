/* rf-scope stress generator, half side — the glue around rf_stress.h:
 * the KS_CMD_RF_STRESS (0xBB) command on UART0 (assembled by the trace's
 * reader, rf_trace_uart.c), a generator task, the sleep veto, the sink.
 *
 * ── The sink: the policy, not the generator, talks to the owner ──────────
 * The generator builds a stress frame and hands it to the sink the half's
 * policy registered — kbd_relay_tx.c on the left, half_link.c on the right.
 * The sink sends it THROUGH THE RADIO OWNER exactly like that half's real
 * frames: same lock, same ESB retries, same target; on a left on USB (chip
 * in PRX) by the same PRX→PTX→PRX excursion as its mode announcement, the
 * FIFO drained into the right-half consumer before leaving. It touches no
 * policy state (no seq, no fallback FSM, no "dongle seen", no supervision
 * clock: a stress frame says nothing of the keyboard). No sink registered
 * (radio absent, half not paired) = the start is refused (ERR_UNKNOWN).
 *
 * ── Task, timer, tickless ────────────────────────────────────────────────
 * Created at the FIRST start, never before: a half that is never stressed
 * pays nothing. Between frames it blocks on its notification, woken by a
 * one-shot esp_timer armed for the scheduler's wait (µs resolution; a
 * one-shot, so no catch-up replay after a sleep); at rest it waits on
 * portMAX_DELAY with the timer stopped — no periodic wait at rest. A running
 * run may tick far faster than 30 ms (that is its point): it always ends on
 * its own (count, duration, the cap of a count-only run, the sleep) and the
 * sleep is vetoed while it runs anyway.
 *
 * ── Sleep ────────────────────────────────────────────────────────────────
 * While a run is active the half posts VEILLE_VETO_STRESS ("stress" in the
 * HB): it does not sleep mid-test. The veto is released when the run stops,
 * whatever stopped it. If the half sleeps anyway, the sleep hook stops the
 * run first (rf_stress_half_sleep). */
#ifndef RF_STRESS_HALF_H
#define RF_STRESS_HALF_H

#include <stdint.h>
#include <stddef.h>
#include "radio_owner.h"

/* Sends one stress frame through the radio owner; the verdict of
 * radio_emettre (ACK / REFUS: went out; INDISPO / PERIME: did not). */
typedef radio_tx_t (*rf_stress_sink_t)(const uint8_t *buf, uint8_t len);

/* The policy, once its radio is up and the half paired. */
void rf_stress_half_set_sink(rf_stress_sink_t sink);

/* The UART0 reader's drain task: a complete KS_CMD_RF_STRESS request.
 * Returns the KS status; on OK fills the 7-byte ack (rf_trace_ctl.h). */
uint8_t rf_stress_half_command(const uint8_t *payload, uint16_t len,
                               uint8_t *ack, size_t cap, size_t *ack_len);

/* The trace's sleep hook, before anything else: a run ends with the sleep.
 * Never blocks. */
void rf_stress_half_sleep(void);

#endif /* RF_STRESS_HALF_H */
