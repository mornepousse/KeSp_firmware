/* Decoding of the Conchodytes mouse's SPDT clicks.
 *
 * Deliberately PURE: levels in, a state out, no ESP-IDF dependency. GPIO
 * reading lives in app/mouse_task.c. This is what makes this reasoning
 * testable on the host (test/test_mouse_input.c), including cases the bench
 * does not produce on demand.
 *
 * The wiring: COM to ground, NO and NC each pulled to 3.3 V by 10 k
 * (R105-R107 and R108-R110 on the board). The firmware reads BOTH contacts of
 * the same button, and it is this pairing that suppresses bounce.
 *
 * WARNING: crossing the pairs — the NO of one button with the NC of another
 * — gives a firmware that appears to work and produces phantom clicks. The
 * pinout is locked by test/test_conchodytes_pins.c for this reason.
 */
#pragma once
#include <stdbool.h>

typedef enum {
    /* NO high, NC low: the moving contact is stuck on NC. */
    MOUSE_CONTACT_RELEASED = 0,
    /* NO low, NC high: the moving contact is stuck on NO. */
    MOUSE_CONTACT_PRESSED,
    /* Both high: the moving contact is stuck on NEITHER. This is the
     * bounce window, and it is OBSERVABLE — that is the whole point of
     * SPDT on a plain switch.
     *
     * WARNING: its DURATION is not measured. The 2026-08-25 campaign was run
     * believing it scanned at 1 kHz, while CONFIG_FREERTOS_HZ is 100 by
     * default: vTaskDelay(1) gave 10 ms, not 1. Over 24 edges, only 4
     * ambiguous samples were seen — so most bounces fell BETWEEN two
     * measurements, which bounds their duration to under 10 ms without
     * pinning it down. What IS established: zero spurious edge over these
     * 24 transitions. To redo with a 1 kHz tick. */
    MOUSE_CONTACT_BOUNCING,
    /* Both low: both contacts closed at the same time, which an
     * inverter does not do. Doubtful wiring or a shorted pin. */
    MOUSE_CONTACT_IMPOSSIBLE,
} mouse_contact_t;

/* Reads the contact state from the two levels, without memory. */
mouse_contact_t mouse_contact_decode(int no_level, int nc_level);

/* Returns the button's next state. On BOUNCING as on IMPOSSIBLE we keep
 * `prev`: concluding nothing is better than inventing an edge. This is
 * where, and nowhere else, debouncing happens — no counter, no
 * delay, no constant to tune. */
bool mouse_button_next(bool prev, mouse_contact_t contact);
