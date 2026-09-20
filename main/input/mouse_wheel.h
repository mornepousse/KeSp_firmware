/* Quadrature decoding of the Conchodytes wheel.
 *
 * The encoder is optical: LD1 lights up a 60-slot wheel, LQ1 (dual
 * phototransistor) yields two channels a quarter period out of phase. The
 * state is `(A << 1) | B`, and a valid step changes ONLY ONE channel at a time.
 *
 * Pure, no ESP-IDF dependency: GPIO reading lives in
 * app/mouse_task.c. Tested on the host by test/test_mouse_input.c.
 *
 * WARNING: THIS DECODING HAS NEVER RUN ON SILICON, and cannot on v1:
 * LQ1 is wired backwards there (power and ground swapped), and above all it
 * only exposes ONE DATA output where quadrature needs two. See
 * Conchodytes/NOTES-V2.md §1bis. This file stays within its own domain — it
 * decodes correct quadrature — but the v1 hardware will never feed it any.
 * Revisit against the v2 schematic once that is settled.
 */
#pragma once
#include <stdint.h>

/* Returns +1, -1, or 0. The 0 covers two distinct cases: no change, and an
 * impossible transition (both channels changed within the same interval, so a
 * missed step). Returning 0 rather than a guessed direction is deliberate —
 * guessing wrong scrolls the page backwards, doing nothing only costs one notch. */
int8_t mouse_wheel_step(uint8_t prev_ab, uint8_t cur_ab);
