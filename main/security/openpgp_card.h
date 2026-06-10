/* main/security/openpgp_card.h — OpenPGP applet dispatch state machine
 * Pure logic: crypto and confirm are injected hooks (host-testable). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Injected dependencies — keeps the applet pure and host-testable,
 * mirroring the otp_proto_hooks_t pattern.
 *
 *  sign(d, hash, n, out, out_n)
 *      Compute a signature using private scalar `d[0..31]` (P-256,
 *      big-endian) over `hash[0..n-1]`, write it to `out`,
 *      set *out_n to the number of bytes written.
 *      Returns true on success, false on error.
 *      On target: delegates to openpgp_crypto (mbedtls).
 *      In tests: records d, returns a canned buffer.
 *
 *  confirm()
 *      0 = not yet (treat as conditions not satisfied in host tests)
 *      1 = user authorized (touch/button)
 *      2 = denied / timed-out
 *      On target: drives sec_confirm.
 *      In tests: returns a preset value.
 */
typedef struct {
    bool     (*sign)(const uint8_t d[32],
                     const uint8_t *hash, uint16_t n,
                     uint8_t *out, uint16_t *out_n);
    int      (*confirm)(void);
} openpgp_card_hooks_t;

/*
 * Initialise the applet: store hook pointers, reset session state
 * (selected flag, PW1/PW3 verified flags), and establish factory PIN
 * baseline in RAM so a fresh boot yields a usable card rather than a
 * blocked one (zero retry counters).  On target, Task 6 NVS load
 * overrides the PIN state after this call.
 * Does NOT wipe or populate the DO store — call
 * openpgp_card_factory_reset() or openpgp_card_ensure_defaults() for that.
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
 * Returns true if all puts succeeded, false if the DO table is full or
 * an entry exceeded OPENPGP_DO_MAX_LEN (should never happen at factory).
 */
bool openpgp_card_ensure_defaults(void);

/*
 * Patch the 4-byte serial field inside the AID DO (bytes [10..13]).
 * On target this is called with the efuse-derived MAC serial (Task 4).
 * If the AID DO is absent it falls back to FACTORY_AID and creates the DO.
 */
void openpgp_card_set_serial(const uint8_t serial[4]);

/*
 * Returns true if a private key has been successfully imported into the
 * Signature slot via PUT DATA 0xDB 3FFF.  False on a fresh / factory-reset card.
 */
bool openpgp_card_key_is_set(void);

/*
 * Process one command APDU (`in[0..in_len-1]`).
 * Writes the response (data + SW1 SW2) into `out[0..out_max-1]`.
 * Returns the number of bytes written (always >= 2 for the SW).
 */
uint16_t openpgp_card_apdu(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max);
