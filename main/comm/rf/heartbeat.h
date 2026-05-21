#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <stdint.h>
#include <stdbool.h>
#include "rf_packet.h"

/* Callbacks invoked by reconciliation to force engine state changes. */
typedef struct {
    void (*force_press)(void *ctx, uint8_t half, uint8_t row, uint8_t col);
    void (*force_release)(void *ctx, uint8_t half, uint8_t row, uint8_t col);
    void *ctx;
} hb_callbacks_t;

#define HB_HALF_LEFT  0
#define HB_HALF_RIGHT 1

/* Per-half tracked state. */
typedef struct {
    uint8_t  local_bitmap[RF_HALF_BITMAP_BYTES];  /* what we think is pressed */
    uint8_t  last_seq;
    uint8_t  last_hb_seq;
    uint32_t last_hb_ms;
    bool     link_up;
    bool     seq_valid;
} hb_half_state_t;

/* Apply a key event from a half to the local bitmap, dedup by seq.
 * Returns true if state changed (caller should re-run the engine). */
bool hb_apply_key(hb_half_state_t *st, const rf_key_event_t *e);

/* Reconcile against a received heartbeat bitmap. Invokes callbacks for
 * any divergence (force press/release). Updates local_bitmap to match. */
void hb_reconcile(hb_half_state_t *st, uint8_t half, const rf_heartbeat_t *h,
                  const hb_callbacks_t *cb, uint32_t now_ms);

/* Call periodically. If now - last_hb_ms > timeout_ms and link was up,
 * release all keys of this half and mark link down. */
void hb_check_timeout(hb_half_state_t *st, uint8_t half,
                      const hb_callbacks_t *cb, uint32_t now_ms, uint32_t timeout_ms);

#endif /* HEARTBEAT_H */
