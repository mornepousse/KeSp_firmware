/* "The matrix has changed" flag — producer/consumer protocol.
 *
 * Producers: the matrix scan callback (priority 5, `matrix_scan.c`) on
 * keyboards, and the RF receive path (`rf_rx_task.c`) on the dongle. Consumer:
 * the keyboard loop (priority 3, `keyboard_task.c`). The producer can preempt
 * the consumer at any instant.
 *
 * ── The rule, and why it is structural ───────────────────────────────────────
 *
 * The consumer must clear the flag BEFORE reading the matrix state, never
 * after. The code used to do the opposite (audit F3):
 *
 *     if (stat_matrix_changed == 1) {
 *         build_keycode_report();     // reads the state — it's slow
 *         stat_matrix_changed = 0;    // cleared AFTER
 *
 * A producer that lands between the two sets a `1` and new data; the `0`
 * wipes it out. The next iteration sees `0` and skips the block: the edge is
 * lost, and nothing will ever re-emit it — the key never reaches the host.
 *
 * By clearing first, the worst case becomes "re-reading a state already
 * processed", which `send_hid_key()`'s deduplication absorbs without emitting
 * anything. Losing a keypress, never. The trade is thus clearly favorable.
 *
 * `matrix_flag_take()` does the test and the clear together: the faulty
 * ordering can no longer be expressed at the call site. That is the whole
 * point of going through this function rather than fixing two lines in place.
 *
 * ── Why neither atomic nor a critical section ────────────────────────────────
 *
 * Writing a `uint8_t` is atomic on ESP32: no word can ever be seen half
 * written. The only interleaving that threatened us was producer-between-
 * read-and-clear, and the ordering alone neutralizes it. A critical section
 * would cost latency on the keystroke path for no further benefit.
 *
 * A theoretical race remains: a producer that sets `1` exactly while
 * `matrix_flag_take()` writes `0` would see its signal lost. The window is a
 * few instructions wide, against several hundred microseconds for the full
 * state read — and the next producer, a millisecond later, will set the flag
 * again. That is the difference between an edge lost on every fast keystroke
 * and an edge lost never observed in practice.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Producer: signals that the matrix state has changed. */
static inline void matrix_flag_signal(volatile uint8_t *flag)
{
    *flag = 1;
}

/* Consumer: test-and-clear. Returns true if there is work to do, and the flag
 * is then already cleared to zero — a producer that arrives during the read
 * that follows will thus be seen on the next pass. */
static inline bool matrix_flag_take(volatile uint8_t *flag)
{
    if (*flag == 0) return false;
    *flag = 0;
    return true;
}
