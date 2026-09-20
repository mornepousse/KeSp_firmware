#pragma once
#include "sdkconfig.h"
#if CONFIG_KASE_DONGLE_FUSION
#include "rf_packet.h"

/* Keymap engine embedded in the dongle — FUSION mode only.
 *
 * The dongle receives both RAW half-matrices (PKT_TYPE_MATRIX), fuses
 * them and runs the shared keymap engine (the same code as the left half), then
 * outputs HID over its USB. See
 * docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md and
 * docs/superpowers/plans/2026-09-13-dongle-fusion-runtime.md.
 *
 * Split of responsibilities under concurrency: rf_rx_task (writer)
 * deposits frames via dongle_engine_on_matrix(); the engine task (reader)
 * consumes them. Only the fusion state is shared — protected by an internal mutex.
 * current_press_* and the engine cycle are touched ONLY by the engine task. */

/* Initializes the engine (tap-hold, tap-dance, combos, leader, overrides, HID) and
 * starts the engine task. Call once, from rf_rx_start(). */
void dongle_engine_start(void);

/* A raw half-matrix just arrived. Routes by half identity and
 * signals a change if the bitmap moved. Safe to call from rf_rx_task. */
void dongle_engine_on_matrix(const rf_matrix_t *m);

/* Phase 2: the left announces its mode. On USB, the dongle stays silent (the left
 * types locally) and re-emits the right; otherwise it types. Called from rf_rx_task
 * (STATUS+USB → true; raw matrix from the left → false). */
void dongle_engine_set_left_usb(bool usb);

/* Is the dongle in "left USB" mode? Read by drain_radio to decide whether to
 * re-emit the right → left. */
bool dongle_engine_left_usb(void);

/* Sync guard rail: the left announced its keymap's fingerprint (config_fp
 * field of PKT_TYPE_STATUS). The dongle remembers it and, on CHANGE,
 * logs the agreement or divergence with its own. Called from
 * rf_rx_task (STATUS of the keyboard slot). */
void dongle_engine_note_left_fp(uint32_t fp);

/* Coherence snapshot for the controller (CDC). own_fp = fingerprint of the
 * dongle's keymap; left_fp = last one announced by the left (0 = never);
 * age_ms = age of that announcement (0xFFFFFFFF = never); match = both
 * agree (equal and non-zero). Each pointer can be NULL. */
void dongle_engine_get_coherence(uint32_t *own_fp, uint32_t *left_fp,
                                 uint32_t *age_ms, bool *match);

/* Auto keymap sync via ACK payload (phase 3). A divergence is KNOWN
 * when the left announced a fingerprint (≠ 0) different from ours: that's
 * the only time the dongle slips something into the ACKs. As soon as the
 * left announces our fingerprint (match=1), silence — zero cost once
 * synchronized. */
/* Diagnostic counter: frames that changed a half's state before
 * the engine consumed the previous change (tap potentially lost). */
uint32_t dongle_engine_transitions_ecrasees(void);
/* Max gap between two engine rounds since the last read (ms), reset to 0. */
uint32_t dongle_engine_gap_max_ms(void);
/* Re-presses of the same key < 30 ms after its release (a half's stale
 * repeat, or mechanical bounce) — CDC RF_STATUS[43..46]. */
uint32_t dongle_engine_reappuis(void);
/* The last re-press: half (RF_HALF_*), key (row*7+col), delay in ms
 * after the release — CDC RF_STATUS[47..50]. */
void dongle_engine_dernier_reappui(uint8_t *half, uint8_t *key, uint16_t *delta_ms);

bool dongle_sync_active(void);

/* Builds the ACK payload to load for the NEXT frame from the left, based
 * on what it requested: req_next < SYNC_N_CHUNKS → the CHUNK
 * req_next (sliced from keymaps[]); otherwise → the BEACON {fingerprint, 40}.
 * Writes into out (≤ 32 bytes), returns the length (0 = nothing). */
uint16_t dongle_sync_ack_for(uint8_t req_next, uint8_t *out);

#endif /* CONFIG_KASE_DONGLE_FUSION */
