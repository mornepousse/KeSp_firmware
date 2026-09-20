#pragma once
/* keymap_pull — dongle keymap pull via ACK payload (left half, fusion).
 *
 * The left half DRIVES: when the ACK of one of its transmissions carries a
 * BEACON (a keymap with a different fingerprint is expected), it starts a
 * pull and requests the next missing chunk (SYNC_REQ) every 100 ms; each
 * chunk arrives in the ACK of the next transmission. Once all 40 are in,
 * the keymap is copied and saved to NVS; the next STATUS announces the
 * new fingerprint and the dongle clears the beacon. A sleep veto holds
 * during the pull. Pure logic lives in keymap_sync.h (tested); this file
 * is the port onto the left half's relay. Extracted from kbd_relay_tx.c on 2026-09-19. */
#include <stdbool.h>
#include <stdint.h>

/* Call with the payload of EVERY ACK received (beacon or chunk), from the
 * transmit path — under the radio owner's lock. */
void keymap_pull_on_ack(const uint8_t *ack, uint8_t n);

/* Is a pull in progress or a keymap pending save? (fast relay cadence,
 * holds take priority) */
bool keymap_pull_en_cours(void);

/* Relay tick, AFTER holds are reaffirmed: saves to NVS
 * once 40/40 are in (copy under the radio lock, NVS outside the lock), then
 * sends a REQ at most every 100 ms while pulling, via `emettre`.
 * Returns true if a REQ went out (the relay is done with its tick). */
bool keymap_pull_tick(void (*emettre)(const uint8_t *, uint8_t));
