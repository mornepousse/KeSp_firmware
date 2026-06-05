/*
 * trackpad_map.c — Pure gesture-to-HID mapping for IQS5xx trackpad.
 *
 * Runs on both the dongle (approach B: mapping moved from half to dongle)
 * and in host tests. No ESP-IDF, no FreeRTOS, no malloc.
 *
 * Gesture logic is transcribed exactly from the original trackpad.c.
 * The only change vs the original is:
 *   - Output type is now trackpad_out_t (not rf_trackpad_t).
 *   - The cursor-scaling block uses an accel curve driven by trackpad_cfg_t.
 *     With the neutral default (base=100, accel=0, gain_max=100) the curve
 *     yields gain=1.0 — identical feel to the previous SENS_NUM/DEN=3/3 code.
 */

#include "trackpad.h"

/* ── Gesture event bitmasks (IQS5xx-B000) ── */
#define IQS5XX_GEST0_SINGLE_TAP     0x01   /* GE0 bit 0 */
#define IQS5XX_GEST0_PRESS_HOLD     0x02   /* GE0 bit 1 */
#define IQS5XX_GEST1_TWO_FINGER_TAP 0x01   /* GE1 bit 0 */
#define IQS5XX_GEST1_SCROLL         0x02   /* GE1 bit 1 */

/* ── Scroll divisor ── */
#define IQS5XX_SCROLL_DIV           8

/* ── Axis inversion (compile-time; default off) ── */
#define IQS5XX_INVERT_X             0
#define IQS5XX_INVERT_Y             0

/* ── HID mouse button bits ── */
#define MOUSE_BTN_LEFT              0x01
#define MOUSE_BTN_RIGHT             0x02
#define MOUSE_BTN_MIDDLE            0x04

/* ── clamp8: clamp signed 16-bit to HID int8 delta range ── */
static inline int8_t clamp8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

/* ── iabs16: absolute value of int16 ── */
static inline int16_t iabs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

/* ── trackpad_map: pure, host-safe ─────────────────────────────────
 *
 * Maps raw IQS5xx gesture fields to trackpad_out_t output.
 * No I/O, no FreeRTOS calls, no global state reads.
 *
 * Gesture precedence (most → least specific):
 *   1. Press-and-hold      → drag: left button held while fingers stay down,
 *                            released on n_fingers=0.
 *   2. Scroll gesture      → scroll_v / scroll_h (both axes), suppresses cursor.
 *   3. Tap (1F/2F/3F)      → button pulse press; next call emits release.
 *   4. Cursor movement     → dx, dy scaled by accel curve.
 *
 * 3-finger tap detection is synthesized: we track the peak number of fingers
 * seen during the current touch session (in state) and route a tap event to
 * MIDDLE if peak >= 3. The chip itself doesn't emit a 3F-tap event.
 *
 * Returns true if a packet should be sent (activity gate passed).
 */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  const trackpad_cfg_t *cfg,
                  trackpad_state_t *state, trackpad_out_t *out)
{
    /* Zero the output */
    out->dx       = 0;
    out->dy       = 0;
    out->buttons  = 0;
    out->scroll_v = 0;
    out->scroll_h = 0;

    bool force_send = false;

    /* ── Axis inversion (compile-time flags; default off) ── */
    if (IQS5XX_INVERT_X) rel_x = (int16_t)(-rel_x);
    if (IQS5XX_INVERT_Y) rel_y = (int16_t)(-rel_y);

    /* ── Track peak fingers across the current touch session ──
     * Resets to 0 below when n_fingers returns to 0. */
    if (n_fingers > state->peak_fingers) state->peak_fingers = n_fingers;

    /* ── Drag: press-and-hold engages drag; releases when fingers leave ── */
    bool press_hold = (ge0 & IQS5XX_GEST0_PRESS_HOLD) != 0;
    bool just_ended_drag = false;
    if (press_hold && !state->drag_active) {
        state->drag_active = true;
        force_send         = true;
    }
    if (state->drag_active) {
        if (n_fingers == 0) {
            /* Fingers lifted → release the drag with one buttons=0 packet */
            state->drag_active = false;
            just_ended_drag    = true;
            force_send         = true;
            /* buttons stays 0x00 → that's the release */
        } else {
            /* Still dragging → hold left button down */
            out->buttons |= MOUSE_BTN_LEFT;
        }
    }

    /* ── Scroll vs cursor (scroll gesture takes priority over movement) ── */
    bool scroll_active = (ge1 & IQS5XX_GEST1_SCROLL) != 0;
    if (scroll_active) {
        /* 2-finger scroll, both axes */
        out->scroll_v = clamp8((int16_t)(rel_y / IQS5XX_SCROLL_DIV));
        out->scroll_h = clamp8((int16_t)(rel_x / IQS5XX_SCROLL_DIV));
        /* dx, dy remain 0 — scroll suppresses cursor */
    } else if (!state->drag_active || n_fingers > 0) {
        /* Cursor movement with accel curve.
         * speed = max(|rel_x|, |rel_y|) — isotropic speed estimate.
         * gain  = base + accel * speed / TRACKPAD_ACCEL_DEN, clamped to [base, gain_max].
         * Neutral default (base=100, accel=0, gain_max=100):
         *   gain = 100 → dx = rel_x * 100 / 100 = rel_x (gain 1.0). */
        int32_t speed = iabs16(rel_x) > iabs16(rel_y) ? iabs16(rel_x) : iabs16(rel_y);
        int32_t gain  = (int32_t)cfg->base + (int32_t)cfg->accel * speed / TRACKPAD_ACCEL_DEN;
        if (gain < cfg->base)     gain = cfg->base;
        if (gain > cfg->gain_max) gain = cfg->gain_max;
        out->dx = clamp8((int16_t)((int32_t)rel_x * gain / 100));
        out->dy = clamp8((int16_t)((int32_t)rel_y * gain / 100));
    }

    /* ── Tap state machine (multi-finger aware) ──
     * - 2-finger tap event (GE1 bit 0) is dedicated → right click.
     * - SingleTap event (GE0 bit 0) is routed by peak_fingers:
     *     peak >= 3 → middle click; otherwise left click.
     * Drag is exclusive: don't synthesize taps in the same iteration as a drag
     * start or just-ended drag (the IQS5xx may emit a SingleTap on hold-release). */
    bool two_finger_tap = (ge1 & IQS5XX_GEST1_TWO_FINGER_TAP) != 0;
    bool single_tap     = (ge0 & IQS5XX_GEST0_SINGLE_TAP)     != 0;

    if (state->pending_release) {
        out->buttons          = 0x00;
        state->pending_release = false;
        force_send             = true;
    } else if (!state->drag_active && !just_ended_drag) {
        if (two_finger_tap) {
            out->buttons           = MOUSE_BTN_RIGHT;
            state->pending_release = true;
            force_send             = true;
        } else if (single_tap) {
            out->buttons           = (state->peak_fingers >= 3)
                                    ? MOUSE_BTN_MIDDLE
                                    : MOUSE_BTN_LEFT;
            state->pending_release = true;
            force_send             = true;
        }
    }

    /* ── Reset peak fingers when surface is empty (new touch starts fresh) ── */
    if (n_fingers == 0) state->peak_fingers = 0;

    /* ── Activity gate ── */
    bool send = force_send
             || (out->dx       != 0)
             || (out->dy       != 0)
             || (out->scroll_v != 0)
             || (out->scroll_h != 0)
             || (out->buttons  != 0x00);

    return send;
}
