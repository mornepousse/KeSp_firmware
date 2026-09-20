/* The dongle's two RF slots.
 *
 * Historically these were the two halves of a single keyboard, and the dongle
 * reconciled their half-matrices. That's no longer the case: the Niphargus master
 * half sends it already-finished HID, and the second slot belongs to the
 * Conchodytes mouse. The two devices no longer have anything in common.
 *
 * Hence this tiny module, whose sole purpose is to make this
 * separation impossible to forget: **losing a slot only releases what
 * that slot held**. Under the old reading, releasing the whole keyboard on the loss
 * of either half was correct; today it would be a bug — a
 * mouse going out of range would wipe out the keystroke in progress.
 *
 * The fallback itself is still necessary: a link that goes silent leaves the host on the
 * last report received. If it was "Super pressed", it stays that way.
 *
 * The pairing NVS keys keep their original names (`mac_left` /
 * `mac_right`): renaming them would unpair hardware already paired for a
 * purely cosmetic gain. Slot 0x01 there designates the keyboard, 0x02 the mouse.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RF_SLOT_KBD    0u   /* Niphargus master half — sends finished HID */
#define RF_SLOT_MOUSE  1u   /* Conchodytes */
#define RF_SLOT_COUNT  2u

/* ── 2.4 GHz channel plan ───────────────────────────────────────────────────
 *
 * Four links coexist. They used to be defined each in its own corner — the
 * pairing channel in rf_pairing.h, the dongle's in the board.h files — and nothing
 * prevented two of them from landing on the same frequency. A collision
 * breaks no compilation: it shows up as a link going silent, or
 * as packets passing through intermittently depending on traffic.
 *
 * They are therefore gathered here, and locked down by test/test_rf_channel_plan.c.
 *
 * Spacing: the driver transmits at 1 Mbps (RF_SETUP = 0x06), and the nRF24L01+
 * Product Specification §6.3 p.25 states that at this rate "the channel occupies
 * a bandwidth of less than 1MHz" — a 1 MHz gap is therefore enough. The 2 MHz
 * constraint only applies at 2 Mbps.
 *
 * Placement: 2.4 GHz WiFi goes up to ~2473 MHz (channel 11). The three
 * data links sit above that, where the band is noticeably
 * quieter. The pairing channel, meanwhile, sits right in WiFi territory — this is deliberate: it
 * is only used for a few seconds, at short range, and under ARC 15.
 *
 * The ISM band stops at 2483.5 MHz. The chip could go up to 2525 (§6.3) but we
 * don't: beyond that we'd jam other services. */
#define RF_CH_KBD_DONGLE    0x4C   /* 2476 MHz — left half → dongle, slot 1 */
#define RF_CH_HALF_LINK     0x4F   /* 2479 MHz — right half → left half  */
#define RF_CH_MOUSE_DONGLE  0x52   /* 2482 MHz — Conchodytes → dongle, slot 2   */
/* The pairing channel lives in rf_pairing.h (RF_PAIR_CHANNEL, 0x28 / 2440 MHz)
 * and stays there: it belongs to the pairing protocol, not to the operating
 * plan. The plan test still includes it in its collision checks. */

/* Address suffixes, 5th byte after the "KaSe" base. 0x01 keyboard and 0x02
 * mouse are those of the dongle's slots (see above); 0x03 designates the
 * inter-half link, which does not go through the dongle. */
#define RF_ADDR_KBD_DONGLE  0x01   /* dongle keyboard slot (BOTH halves under fusion) */
#define RF_ADDR_HALF_LINK   0x03

/* ── Keyboard → dongle link supervision ───────────────────────────────────
 *
 * The dongle declares a slot LOST after RF_LINK_LOST_MS of total silence and
 * releases its keys — otherwise a vanished keyboard would leave a stuck
 * key on the host. So it expects an idle status frame.
 *
 * But PKT_TYPE_STATUS was NEVER sent: it only existed in decoding. As long
 * as typing continues, HID reports keep the link alive by accident; but a
 * HELD key produces no change, hence no more reports, and the
 * dongle would release the key after ~2 s. Found on the bench on 2026-09-08.
 *
 * Both constants are here, and not each on its own side: it's a contract
 * between two firmwares, and the half that transmits must know the budget of the one
 * that listens. Locked down by test/test_rf_status_cadence.c. */
#define RF_STATUS_PERIOD_MS  1000u   /* status frame cadence */
#define RF_BATT_PERIOD_MS    30000u  /* slow STATUS from the RIGHT at rest (gauge) — the voltage
                                      * of a 16340 doesn't move in 30 s; contract with the dongle */
#define RF_REARM_SILENCE_MS  2000u   /* silence → rewrite the RX config (frozen radio) */
#define RF_LINK_LOST_MS      2500u   /* silence → slot lost, fallback applied */

/* Should a status frame be sent now? `dernier_ms` is the date of the
 * LAST transmission of any kind — an HID report keeps the link alive
 * just as well as a status frame, no need to add one while typing.
 * Gap in unsigned arithmetic: the ms counter wraps around after 49 days. */
static inline bool rf_status_doit_emettre(uint32_t now_ms, uint32_t dernier_ms,
                                          uint32_t periode_ms)
{
    return (uint32_t)(now_ms - dernier_ms) >= periode_ms;
}

typedef enum {
    RF_SAFE_NONE = 0,
    RF_SAFE_RELEASE_KEYS,      /* empty keyboard report */
    RF_SAFE_RELEASE_BUTTONS,   /* zeroed mouse report */
} rf_safe_action_t;

typedef struct {
    uint32_t last_rx_ms;
    bool     up;
} rf_slot_link_t;

/* Any packet received — heartbeat, status, HID report — counts as proof of life. */
static inline void rf_slot_link_rx(rf_slot_link_t *l, uint32_t now_ms)
{
    if (l == NULL) return;
    l->last_rx_ms = now_ms;
    l->up = true;
}

/* To be called on every turn of the RF loop. Returns the fallback action to
 * perform, only once per loss: the loop runs every 10 ms, a repeated
 * trigger would flood the HID endpoint with empty reports.
 *
 * The subtraction is unsigned by design: the millisecond counter
 * wraps around after ~49 days, which a permanently plugged-in dongle
 * reaches. */
static inline rf_safe_action_t rf_slot_link_check(rf_slot_link_t *l, uint8_t slot,
                                                  uint32_t now_ms, uint32_t timeout_ms)
{
    if (l == NULL || !l->up) return RF_SAFE_NONE;
    if ((uint32_t)(now_ms - l->last_rx_ms) < timeout_ms) return RF_SAFE_NONE;
    l->up = false;
    return (slot == RF_SLOT_MOUSE) ? RF_SAFE_RELEASE_BUTTONS : RF_SAFE_RELEASE_KEYS;
}
