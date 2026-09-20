#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rf_packet.h"   /* half-matrix bitmap + its accessors */

/* Niphargus inter-half radio link — brick B3.
 *
 * The RIGHT half transmits its half-matrix, the LEFT listens to it. The right
 * has neither a keymap engine nor HID output: the spec describes it as "a
 * scanner that reports its raw matrix". The left fuses what it receives with
 * its own scan — columns 0-6 for itself, 7-13 for the right, hence
 * KEYMAP_COLS = 2 × MATRIX_COLS on the master.
 *
 * Channel 0x4F (2479 MHz), address KaSe.03 — see the channel plan in
 * rf_slot.h. Frame: PKT_TYPE_HEARTBEAT, which already carries the
 * half-matrix bitmap, the battery gauge and a sequence number.
 *
 * ⚠ This module does NOT address risk R1 — the left cannot listen
 * while it transmits to the dongle. The PRX/PTX switch is the next
 * step; we first prove the link carries, otherwise a switch failure
 * would be indistinguishable from a link that doesn't work. */

/* ── State of the remote half, as seen by the master (pure logic) ───────────
 *
 * Tested on host in test/test_half_state.c, modeled on the inlines of
 * comm/usb/usb_presence.h.
 *
 * The state is ABSOLUTE, not differential: every frame carries the entire
 * matrix, so a lost frame is caught up by the next one with no accumulation
 * or drift. This is what makes the link tolerant to the losses R1 measures.
 *
 * ⚠ The fallback on silence is the dangerous part. A half that goes out of
 * range or whose battery dies would leave the host on the last state received —
 * and if that was "Shift held", it stays held. So we release after a
 * silence, but ONLY what that half was holding: rf_slot.h warns
 * that a mistargeted release would be worse than the disease. */
typedef struct {
    uint8_t  bitmap[RF_HALF_BITMAP_BYTES];  /* last state received */
    uint32_t derniere_ms;                   /* when it was received */
    bool     vivant;                        /* false = silence already observed */
} half_state_t;

/* A frame just arrived: it replaces the previous state. */
static inline void half_state_recu(half_state_t *st, const uint8_t *bitmap,
                                   uint32_t now_ms)
{
    memcpy(st->bitmap, bitmap, RF_HALF_BITMAP_BYTES);
    st->derniere_ms = now_ms;
    st->vivant = true;
}

/* Called periodically. Returns true EXACTLY ONCE, the moment the silence
 * exceeds the delay: that's the signal "release what this half was holding".
 * Subsequent calls return false as long as no frame has come back —
 * otherwise the engine would release, every cycle, keys already released. */
static inline bool half_state_timeout(half_state_t *st, uint32_t now_ms,
                                      uint32_t delai_ms)
{
    if (!st->vivant) return false;
    if ((uint32_t)(now_ms - st->derniere_ms) < delai_ms) return false;
    memset(st->bitmap, 0, RF_HALF_BITMAP_BYTES);
    st->vivant = false;
    return true;
}

/* Is the (row, col) key of the remote half pressed? Coordinates are
 * LOCAL to that half; the offset toward keymap columns 7-13 is
 * the caller's responsibility. */
static inline bool half_state_pressed(const half_state_t *st, uint8_t row,
                                      uint8_t col)
{
    return rf_bitmap_get(st->bitmap, row, col);
}

/* ── Geometry: where to place the remote half's columns ────────────────────
 *
 * Tested on host in test/test_half_col_map.c.
 *
 * Both halves are the same PCB flipped over: column 0 of the left is its
 * LEFTMOST key, so by symmetry column 0 of the right is its
 * RIGHTMOST key. A plain `col + 7` offset would then place the right
 * half backwards — typing the home row produces ";lkjh".
 *
 * The mirroring is a property of the WIRING, not the protocol: the right
 * emits its physical coordinates and doesn't need to know where it's placed.
 * The conversion therefore belongs to the master, and the flag comes from its
 * board.h (BOARD_REMOTE_COLS_MIRRORED). */
static inline uint8_t half_col_to_keymap(uint8_t col, uint8_t cols, bool miroir)
{
    return miroir ? (uint8_t)(2u * cols - 1u - col)
                  : (uint8_t)(col + cols);
}

/* ── Fusion of TWO half-matrices at the dongle ("dongle fusion" brick) ──────
 *
 * Tested on host in test/test_fuse_halves.c. Design:
 * docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md.
 *
 * In wireless mode, both halves transmit their RAW matrix; the dongle
 * fuses them and runs the engine. Neither half is "local" here,
 * unlike matrix_apply_remote: we receive two bitmaps and produce the
 * (row, keymap column) positions the engine indexes. Left = columns 0..cols-1
 * directly; right = high columns via half_col_to_keymap (PCB mirror).
 *
 * Pure: no I/O, no global state. Writes into out_row/out_col, returns the
 * number of keys, bounded to max. */
static inline uint8_t fuse_halves(const uint8_t *left_bm, const uint8_t *right_bm,
                                  uint8_t cols, bool right_mirror,
                                  uint8_t *out_row, uint8_t *out_col, uint8_t max)
{
    uint8_t n = 0;
    for (uint8_t r = 0; r < RF_HALF_ROWS && n < max; r++)
        for (uint8_t c = 0; c < cols && n < max; c++)
            if (rf_bitmap_get(left_bm, r, c)) {
                out_row[n] = r; out_col[n] = c; n++;   /* left: direct column */
            }
    for (uint8_t r = 0; r < RF_HALF_ROWS && n < max; r++)
        for (uint8_t c = 0; c < cols && n < max; c++)
            if (rf_bitmap_get(right_bm, r, c)) {
                out_row[n] = r;
                out_col[n] = half_col_to_keymap(c, cols, right_mirror);
                n++;
            }
    return n;
}

/* ── Fusion state at the dongle: TWO half-matrices routed by identity ───────
 *
 * Tested on host in test/test_fusion_state.c.
 *
 * On the master, only one half was remote (a single half_state_t): the other
 * was its own local matrix. At the dongle there is NO local matrix at all — both
 * halves are remote and arrive on the same keyboard slot, distinguished
 * by PKT_TYPE_MATRIX's identity byte. So we hold two half-states, we
 * route each frame to the right one, and we expire each independently: a
 * silent half must not release what the other is holding (the same caution as
 * rf_slot.h on the supervision side).
 *
 * Pure: no I/O, no global state. Reuses half_state_* and fuse_halves. */
typedef struct {
    half_state_t left;
    half_state_t right;
} fusion_state_t;

/* Stores a decoded frame into its half's half-state. Returns false if
 * the identity is neither left nor right — a corrupted frame must not
 * write anything. */
static inline bool fusion_apply(fusion_state_t *fs, const rf_matrix_t *m,
                                uint32_t now_ms)
{
    if (m->half == RF_HALF_LEFT)  { half_state_recu(&fs->left,  m->bitmap, now_ms); return true; }
    if (m->half == RF_HALF_RIGHT) { half_state_recu(&fs->right, m->bitmap, now_ms); return true; }
    return false;
}

/* Expires both half-states. Returns true if at least one just fell into
 * silence (signal "recompute the report"). BOTH are evaluated — no
 * short-circuit —, otherwise a half would never expire when the other
 * already is. Like half_state_timeout, signals only once per silence. */
static inline bool fusion_timeout(fusion_state_t *fs, uint32_t now_ms,
                                  uint32_t delai_ms)
{
    bool l = half_state_timeout(&fs->left,  now_ms, delai_ms);
    bool r = half_state_timeout(&fs->right, now_ms, delai_ms);
    return l || r;
}

/* Produces the fused list (row, keymap column) that the engine will index.
 * Left in direct columns, right in high columns via the PCB mirror. */
static inline uint8_t fusion_collect(const fusion_state_t *fs, uint8_t cols,
                                     bool right_mirror, uint8_t *out_row,
                                     uint8_t *out_col, uint8_t max)
{
    return fuse_halves(fs->left.bitmap, fs->right.bitmap, cols, right_mirror,
                       out_row, out_col, max);
}

/* ── Cadence: when must the right half transmit? ────────────────────────────
 *
 * Tested on host in test/test_half_tx_cadence.c.
 *
 * Two rules written separately contradicted each other: the right only
 * transmitted on CHANGE (design premise §2.3, and the only one that makes R1
 * tenable), while the left RELEASES after a silence. Holding a key
 * produces no change, hence no frame — and the left would release a
 * key that was still pressed. No repeat, and the right's modifiers
 * would let go mid-keystroke.
 *
 * The correct rule distinguishes REST from INACTIVITY: silent when nothing is
 * pressed, refreshed as long as something is. Rest still costs
 * nothing, but a hold is reaffirmed before the left can doubt it.
 *
 * ⚠ The two constants are linked: AT LEAST one refresh must be
 * losable without the delay expiring, otherwise a single missed packet releases
 * a held key. The test verifies this. */
#define HALF_TX_REFRESH_MS   100u
/* 400 ms, not 250: at a 100 ms refresh, the margin was only
 * one packet. It now takes four consecutive ones to release incorrectly.
 * A truly dead half still unlocks in under half a
 * second, which remains imperceptible. */
#define HALF_LINK_TIMEOUT_MS 400u

static inline bool half_tx_doit_emettre(bool change, bool tenu,
                                        uint32_t now_ms, uint32_t dernier_ms,
                                        uint32_t periode_ms)
{
    if (change) return true;
    if (!tenu)  return false;   /* rest: silence, that's R1's premise */
    /* Gap in unsigned arithmetic: the ms counter overflows at ~49 days
     * and a signed subtraction would freeze transmission that day. */
    return (uint32_t)(now_ms - dernier_ms) >= periode_ms;
}

/* ── Bounded repair after a change ───────────────────────────────────────────
 *
 * Tested on host in test/test_half_tx_repeat.c.
 *
 * half_tx_doit_emettre only replays state for a HOLD. A change frame
 * rejected by the ESB (15 retransmissions exhausted, ~1% on the bench)
 * therefore had only one chance: a brief tap that got lost was never repaired
 * (bench 2026-09-13, same cause as the left). Here a change ARMS a bounded
 * number of repeats, consumed one per tick of the refresh task
 * (20 ms) even if nothing is held; then back to the hold rule. Rest,
 * never armed, stays silent — the §2.3 (R1) premise holds. The dongle
 * deduplicates by content: the repeats cost it nothing. */
#define HALF_TX_REPEATS 3u   /* 3 × 20 ms ≈ the left's 5 × 10 ms window */

typedef struct { uint8_t restantes; } half_tx_repeat_t;

static inline bool half_tx_doit_emettre_repare(half_tx_repeat_t *rep, bool change,
                                               bool tenu, uint32_t now_ms,
                                               uint32_t dernier_ms, uint32_t periode_ms)
{
    if (change) { rep->restantes = HALF_TX_REPEATS; return true; }
    if (rep->restantes) { rep->restantes--; return true; }
    return half_tx_doit_emettre(false, tenu, now_ms, dernier_ms, periode_ms);
}

/* ── Right half's transmission target: dongle ↔ direct left (fusion) ────────
 *
 * In fusion, the right talks to the DONGLE'S KEYBOARD SLOT (KaSe.01), which
 * runs the engine. But if the user UNPLUGS the dongle and types on the
 * left over USB, the right no longer has anyone: its frames to the dongle no
 * longer get acknowledged, and left-USB — which is already listening on KaSe.03
 * for the dongle's re-emission — hears nothing anymore. Fallback: on N
 * consecutive sends WITHOUT ACK, the right RE-ARMS its chip and SWITCHES to the
 * other listener. Left-USB is already listening on KaSe.03; the right then
 * transmits the pre-fusion HEARTBEAT that kbd_relay decodes (the fusion MATRIX,
 * on the other hand, is only decoded by the dongle). If the dongle comes back —
 * or if left USB goes away and the left stops listening — the current target
 * stops acknowledging and we switch back: self-healing, never two targets at
 * once, so never a double keystroke.
 *
 * ⚠ Silent at rest: the right only transmits on change/hold (see
 * half_tx_doit_emettre), so the counter only advances when there is something
 * to route. The switch also re-arms a stuck chip (nRF24 clone) by
 * rewriting the PTX config — it subsumes the old "re-arm in place" watchdog.
 *
 * Tested on host in test/test_half_tx_target.c. */
typedef enum {
    HALF_TX_TO_DONGLE = 0,   /* MATRIX to KaSe.01 — the dongle fuses and types */
    HALF_TX_TO_LEFT   = 1,   /* HEARTBEAT to KaSe.03 — left-USB types */
} half_tx_target_t;

typedef struct {
    half_tx_target_t cible;    /* current listener */
    uint16_t         sans_ack; /* consecutive unacknowledged sends */
} half_tx_fsm_t;

/* 8 sends: each retransmitted up to 15 times in hardware (ESB), so ~120
 * lost HW attempts before concluding "this listener is gone" — confident
 * enough not to switch on a glitch, short enough for the switch to be felt
 * in <1 s on a hold (refreshed ~10/s). */
#define HALF_TX_SWITCH_FAILS 8u

/* A send just happened (ack = acknowledged). Updates the state and returns
 * true if the PTX chip must be RE-ARMED on the (new) target `s->cible`. */
static inline bool half_tx_target_step(half_tx_fsm_t *s, bool ack, uint16_t seuil)
{
    if (ack) { s->sans_ack = 0; return false; }
    if (++s->sans_ack >= seuil) {
        s->sans_ack = 0;
        s->cible = (s->cible == HALF_TX_TO_DONGLE) ? HALF_TX_TO_LEFT
                                                   : HALF_TX_TO_DONGLE;
        return true;
    }
    return false;
}

/* Transmitter — right half. Initializes the radio in PTX on the link's channel.
 * Returns false if the radio doesn't respond. */
bool half_link_tx_init(void);

/* Transmits the half-matrix's current state. The sequence number is managed
 * internally. Returns true if the packet was acknowledged by the left. */
bool half_link_tx_matrix(const uint8_t *bitmap);
/* Same thing for a REPEAT: `encore_valide` evaluated under the radio lock,
 * nothing goes out if the state is stale (see radio_emettre). */
#include "radio_owner.h"
bool half_link_tx_matrix_si(const uint8_t *bitmap, radio_valide_cb_t encore_valide, void *ctx);
#if CONFIG_KASE_BATT_SENSE
/* Gauge: a STATUS (voltage, charge state, RIGHT identity) toward the current
 * target. Called by the refresh task every RF_BATT_PERIOD_MS
 * and on wake. Returns the ACK. */
bool half_link_tx_status(void);
/* Display: the current target is the dongle and the LAST transmission was
 * acknowledged (sticky: a half is silent at rest; false when fallen back to
 * the left). */
bool half_link_tx_dongle_vu(void);
#endif

/* Applies the cadence rule then transmits if warranted.
 *
 * The scan callback calls (bitmap, true): something new, to publish right
 * away. The refresh task calls (NULL, false): it does NOT provide
 * state, it reaffirms what's already recorded. This is what keeps
 * a single writer — otherwise it would rewrite a state read on the previous
 * round and resurrect a release published in the meantime. */
void half_link_tx_update(const uint8_t *bitmap, bool change);

/* Starts the task that reaffirms holds. Without it, a key held
 * longer than HALF_LINK_TIMEOUT_MS is wrongly released by the left: the
 * driver's callback only fires on change, so a hold produces no
 * frame at all. */
bool half_link_tx_refresh_start(void);


