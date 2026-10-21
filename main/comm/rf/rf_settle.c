/* Dongle RX settle — see rf_settle.h. PURE. */
#include "rf_settle.h"

uint32_t rf_settle_remaining_us(uint32_t now_us, uint32_t last_irq_us, uint32_t settle_us)
{
    uint32_t elapsed = now_us - last_irq_us;   /* modulo 2^32: wrap-safe */
    return (elapsed >= settle_us) ? 0u : settle_us - elapsed;
}

uint32_t rf_settle_gate(const rf_settle_t *s, uint32_t settle_us,
                        rf_settle_clock_fn now, rf_settle_wait_fn wait, void *ctx)
{
    if (settle_us == 0) return 0;
    uint32_t last = s->last_irq_us;            /* the stamp BEFORE the clock */
    uint32_t rem = rf_settle_remaining_us(now(ctx), last, settle_us);
    if (rem) wait(ctx, rem);
    return rem;
}
