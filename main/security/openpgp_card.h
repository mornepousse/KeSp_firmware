/* main/security/openpgp_card.h — OpenPGP applet dispatch state machine
 * Pure logic: crypto and confirm are injected hooks (host-testable). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Injected dependencies — keeps the applet pure and host-testable,
 * mirroring the otp_proto_hooks_t pattern.
 *
 *  sign(hash, n, out, out_n)
 *      Compute a signature over `hash[0..n-1]`, write it to `out`,
 *      set *out_n to the number of bytes written.
 *      Returns true on success, false on error.
 *      On target: delegates to openpgp_crypto (mbedtls).
 *      In tests: returns a canned buffer.
 *
 *  confirm()
 *      0 = not yet (treat as conditions not satisfied in host tests)
 *      1 = user authorized (touch/button)
 *      2 = denied / timed-out
 *      On target: drives sec_confirm.
 *      In tests: returns a preset value.
 */
typedef struct {
    bool     (*sign)(const uint8_t *hash, uint16_t n,
                     uint8_t *out, uint16_t *out_n);
    int      (*confirm)(void);
} openpgp_card_hooks_t;

/*
 * Initialise the applet, reset all state (PIN flags, retry counters,
 * selected flag, DO store), and store the hook pointers.
 * Must be called once before openpgp_card_apdu().
 */
void openpgp_card_init(const openpgp_card_hooks_t *hooks);

/*
 * Process one command APDU (`in[0..in_len-1]`).
 * Writes the response (data + SW1 SW2) into `out[0..out_max-1]`.
 * Returns the number of bytes written (always >= 2 for the SW).
 */
uint16_t openpgp_card_apdu(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max);
