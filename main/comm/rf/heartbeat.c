/*
 * Heartbeat-based state reconciliation for the dongle RF link.
 * Pure logic (no ESP-IDF deps) so it is unit-tested host-side.
 */
#include "heartbeat.h"
#include <string.h>

bool hb_apply_key(hb_half_state_t *st, const rf_key_event_t *e)
{
    /* Bound row/col BEFORE any bitmap access: rf_decode_key extracts 4-bit
     * fields (0..15) straight from the wire, but local_bitmap is sized for
     * RF_HALF_ROWS×RF_HALF_COLS only. An out-of-range RF frame would otherwise
     * read/write up to 10 bytes past local_bitmap into adjacent state/BSS.
     * (Pentest 2026-06-25, OTA OOB write.) */
    if (e->row >= RF_HALF_ROWS || e->col >= RF_HALF_COLS)
        return false;
    if (st->seq_valid && e->seq == st->last_seq && !e->is_retry)
        return false;                     /* duplicate */
    st->last_seq = e->seq;
    st->seq_valid = true;
    bool cur = rf_bitmap_get(st->local_bitmap, e->row, e->col);
    if (cur == e->pressed) return false;  /* no state change */
    rf_bitmap_set(st->local_bitmap, e->row, e->col, e->pressed);
    return true;
}

void hb_reconcile(hb_half_state_t *st, uint8_t half, const rf_heartbeat_t *h,
                  const hb_callbacks_t *cb, uint32_t now_ms)
{
    for (uint8_t row = 0; row < RF_HALF_ROWS; row++) {
        for (uint8_t col = 0; col < RF_HALF_COLS; col++) {
            bool want = rf_bitmap_get(h->bitmap, row, col);
            bool have = rf_bitmap_get(st->local_bitmap, row, col);
            if (want == have) continue;
            if (want) {
                rf_bitmap_set(st->local_bitmap, row, col, true);
                if (cb && cb->force_press) cb->force_press(cb->ctx, half, row, col);
            } else {
                rf_bitmap_set(st->local_bitmap, row, col, false);
                if (cb && cb->force_release) cb->force_release(cb->ctx, half, row, col);
            }
        }
    }
    st->last_hb_seq = h->seq;
    st->last_hb_ms = now_ms;
    st->link_up = true;
}

void hb_check_timeout(hb_half_state_t *st, uint8_t half,
                      const hb_callbacks_t *cb, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!st->link_up) return;
    if (now_ms - st->last_hb_ms <= timeout_ms) return;

    /* Release everything this half holds */
    for (uint8_t row = 0; row < RF_HALF_ROWS; row++)
        for (uint8_t col = 0; col < RF_HALF_COLS; col++)
            if (rf_bitmap_get(st->local_bitmap, row, col)) {
                rf_bitmap_set(st->local_bitmap, row, col, false);
                if (cb && cb->force_release) cb->force_release(cb->ctx, half, row, col);
            }
    st->link_up = false;
}
