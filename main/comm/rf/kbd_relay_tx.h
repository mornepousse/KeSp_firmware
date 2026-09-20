#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * kbd_relay_tx.h — Smart keyboard NRF24 HID relay (PTX to dongle).
 *
 * When CONFIG_KASE_KBD_WIRELESS=y, a standalone keyboard relays its final HID
 * reports over NRF24 to the M.2 dongle (PKT_TYPE_HIDREPORT packets) instead of
 * delivering them locally over USB/BLE.
 *
 * This module owns the NRF radio handle (PTX), the pairing NVS state, and the
 * two low-level TX helpers called by hid_report.c under the CONFIG guard.
 */

/* ── Bounded repeat of the last report (pure logic, host-tested) ──────
 *
 * The HIDREPORT path has no reconciliation: a key-up lost on a noisy
 * radio link would leave a key stuck. The relay therefore re-sends the last
 * report after each change — but a BOUNDED number of times, then goes quiet.
 *
 * The original implementation re-sent indefinitely from the board's first
 * keystroke ever (`s_have_last` was never reset to false). Measured on the
 * bench on 2026-09-05: 100 packets/s with the keyboard idle. Two consequences —
 * the Niphargus left half, deaf while it transmits, was so permanently,
 * which ruins the design's R1 bet; and B7's < 50 µA budget
 * became unreachable.
 *
 * Pure, hence host-testable (test/test_kbd_refresh.c), on the model of
 * kbd_route_target/vbus_debounce_step in comm/usb/usb_presence.h. */
typedef struct {
    uint8_t left;          /* repeats remaining */
} kbd_refresh_t;

/* HID state change: re-arms the repeat counter. */
static inline void kbd_refresh_arm(kbd_refresh_t *r, uint8_t repeats)
{
    r->left = repeats;
}

/* One tick of the refresh timer: should we re-send now?
 * Consumes one repeat when the answer is yes. */
static inline bool kbd_refresh_step(kbd_refresh_t *r)
{
    if (r->left == 0) return false;
    r->left--;
    return true;
}

/* Refresh timer cadence (pure, test/test_kbd_refresh.c).
 * 10 ms as long as there is something to repeat, a key held, a sync in
 * progress — or the left LISTENS to the right re-sent by the dongle (USB
 * route): it's this tick that drains the receive FIFO (3 frames); at 100 ms
 * the press and release of a right-half key fell in the same round and only
 * the release survived (bench 2026-09-16). 100 ms otherwise (idle, DFS). */
#include "cadence.h"   /* KBD_RELAY_REFRESH_MS / KBD_RELAY_REPOS_MS */
static inline uint32_t kbd_relay_cadence_ms(bool reparation, bool tenu, bool sync, bool ecoute_usb)
{
    return (reparation || tenu || sync || ecoute_usb) ? KBD_RELAY_REFRESH_MS : KBD_RELAY_REPOS_MS;
}

/* Init NRF radio in PTX mode and restore (or discover) the dongle pairing from
 * NVS, declaring device type RF_DEV_SMART_KBD. Sets the internal s_paired flag.
 * Safe to call even if the board has no NRF hardware — the flag stays false. */
void kbd_relay_init(void);

/* Returns true when wireless mode is active AND the radio is paired to a dongle.
 * False ⇒ hid_report.c falls through to the local USB/BLE path. */
bool kbd_relay_active(void);
/* Display: the LATEST transmission to the dongle was acknowledged (sticky). */
bool kbd_relay_dongle_vu(void);

/* Encode + transmit a keyboard HID report (PKT_TYPE_HIDREPORT / RF_HID_SUB_KBD).
 * modifier: standard HID modifier byte. kb[6]: keycodes (modifiers already
 * extracted by hid_report.c's extract_modifiers before this is called). */
void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6]);

/* Encode + transmit a mouse HID report (PKT_TYPE_HIDREPORT / RF_HID_SUB_MOUSE). */
void kbd_relay_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Fusion (KASE_DONGLE_FUSION): transmit the RAW half-matrix to the dongle
 * (PKT_TYPE_MATRIX), instead of the finished HID. `half` = RF_HALF_LEFT/RIGHT;
 * `bitmap` = RF_HALF_BITMAP_BYTES bytes. Goes through the same send path
 * as kbd_relay_send_kbd (excursion if the radio is listening, direct otherwise). */
void kbd_relay_send_matrix(uint8_t half, const uint8_t *bitmap);

/* Fusion phase 2 (4b): state of the REMOTE (right) half-matrix re-sent by the
 * dongle and received while listening over USB. The left's engine reads it to
 * merge the right into the high columns (matrix_apply_remote). Mirrors
 * what the pre-fusion master used to read from half_link. */
bool kbd_relay_remote_pressed(uint8_t row, uint8_t col);
bool kbd_relay_remote_changed(void);

/* Light-sleep hooks: stop the refresh timer + power the NRF down (holding the TX
 * mutex) before sleep; power up + restart the timer on wake. */
void kbd_relay_sleep_prepare(void);
void kbd_relay_wake_restore(void);
