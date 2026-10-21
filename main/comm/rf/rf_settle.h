/* Dongle RX settle: let the nRF24's auto-ACK leave before touching the chip.
 *
 * PURE (host-tested, test/test_rf_settle.c): no ESP-IDF include. The device
 * side (the clock, the waiter, the ISR) lives in rf_rx_task.c.
 *
 * Why: on the clone nRF24 modules, an SPI access to the PRX while it sends
 * its auto-ACK makes the ACK miss. The PRX raises RX_DR (IRQ) at the end of
 * a frame, turns around (~130 us) and sends the ACK (up to ~460 us after the
 * IRQ with a 32-byte ACK payload at 1 Mbit/s). Reading at once made ~90 % of
 * first ACKs miss; 500 us after the IRQ, first-attempt success went 6 % ->
 * 98.5 % (rf_scope stress, 2026-10-21).
 *
 * The rule (review C1/I1/I3 of 88d49728, which waited 500 us UNCONDITIONALLY
 * at the top of every pass of both radios — half of core 0 at rest, the
 * engine starved under a 1 kHz mouse):
 *  - each radio carries the time of ITS last IRQ, stamped by the GPIO ISR
 *    (rf_settle_note_irq, arg = the radio);
 *  - before EACH SPI access sequence on a radio (each frame read of a drain,
 *    the ACK-payload load, an excursion, a pairing read) the rx task waits
 *    what remains of settle_us since that radio's last IRQ (rf_settle_gate);
 *  - nothing when the IRQ is older than settle_us — a pass woken by the 1 ms
 *    timer with no recent IRQ waits nothing — and never for the other
 *    radio's IRQ.
 *
 * Clock: 32-bit microseconds (esp_timer truncated), wrap-safe for any
 * elapsed time below 71.6 min. A stamp older than that aliases: once per
 * wrap a silent radio may wait at most settle_us once — bounded, harmless.
 * The caller reads the stamp BEFORE the clock (rf_settle_gate does): an IRQ
 * landing between the two reads is then the "arrived during an SPI access"
 * residual (see rf_rx_task.c), never a negative elapsed time.
 */
#ifndef RF_SETTLE_H
#define RF_SETTLE_H

#include <stdint.h>

typedef struct {
    volatile uint32_t last_irq_us;   /* written by the GPIO ISR only */
} rf_settle_t;

static inline void rf_settle_init(rf_settle_t *s) { s->last_irq_us = 0; }

/* ISR side: one 32-bit store (atomic on the target), no call. */
static inline void rf_settle_note_irq(rf_settle_t *s, uint32_t now_us) { s->last_irq_us = now_us; }

/* How long to wait before an SPI access: settle_us - (now - last), or 0 once
 * settle_us has elapsed (or settle_us == 0). Wrap-safe. */
uint32_t rf_settle_remaining_us(uint32_t now_us, uint32_t last_irq_us, uint32_t settle_us);

typedef uint32_t (*rf_settle_clock_fn)(void *ctx);
typedef void     (*rf_settle_wait_fn)(void *ctx, uint32_t us);

/* Before an SPI access sequence on the radio owning `s`: reads its stamp,
 * then the clock, and calls wait(ctx, remaining) only when something
 * remains. Returns the wait asked (0 = none, the waiter was not called). The
 * wait is computed ONCE from the stamp read: an IRQ arriving during the wait
 * is not chased (forward progress under any frame rate — every gate ends
 * after at most settle_us). */
uint32_t rf_settle_gate(const rf_settle_t *s, uint32_t settle_us,
                        rf_settle_clock_fn now, rf_settle_wait_fn wait, void *ctx);

#endif /* RF_SETTLE_H */
