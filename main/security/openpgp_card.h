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
 * Initialise the applet: store hook pointers and reset session state
 * (selected flag, PW1/PW3 verified flags).  Does NOT wipe the DO store
 * or restore factory PINs — call openpgp_card_factory_reset() for that.
 * Must be called once before openpgp_card_apdu().
 */
void openpgp_card_init(const openpgp_card_hooks_t *hooks);

/*
 * Wipe all DOs, restore factory PINs / retry counters, reset session
 * state, and repopulate all factory-default DOs.
 * Safe to call at any time; used by tests and future admin commands.
 */
void openpgp_card_factory_reset(void);

/*
 * Populate any *missing* factory-default DOs without touching existing
 * ones.  Called automatically by openpgp_card_factory_reset(); may also
 * be called at target boot after openpgp_do_init() loads NVS state.
 */
void openpgp_card_ensure_defaults(void);

/*
 * Patch the 4-byte serial field inside the AID DO (bytes [10..13]).
 * On target this is called with the efuse-derived MAC serial (Task 4).
 * Has no effect if the AID DO has not been initialised yet.
 */
void openpgp_card_set_serial(const uint8_t serial[4]);

/*
 * Process one command APDU (`in[0..in_len-1]`).
 * Writes the response (data + SW1 SW2) into `out[0..out_max-1]`.
 * Returns the number of bytes written (always >= 2 for the SW).
 */
uint16_t openpgp_card_apdu(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max);
