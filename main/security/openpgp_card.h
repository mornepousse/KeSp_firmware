/* main/security/openpgp_card.h — OpenPGP applet dispatch state machine
 * Pure logic: crypto and confirm are injected hooks (host-testable). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Key slots (OpenPGP: Signature / Decryption / Authentication) */
enum {
    OPENPGP_SLOT_SIG = 0,
    OPENPGP_SLOT_DEC = 1,
    OPENPGP_SLOT_AUT = 2,
    OPENPGP_SLOT_COUNT = 3,
};

/* Slot algorithm ids — match the leading byte of algo attrs C1/C2/C3.
 * 0x12 (ECDH) always means X25519 in this implementation: C2 only accepts
 * the cv25519 OID (Task 2), ECDH-P256 is not supported. */
#define PGP_ALGO_ECDSA_P256  0x13u
#define PGP_ALGO_ECDH        0x12u

/*
 * Injected dependencies — keeps the applet pure and host-testable,
 * mirroring the otp_proto_hooks_t pattern.  On target the hooks delegate to
 * openpgp_crypto (mbedtls); in host tests they record arguments and return
 * canned buffers.
 */
typedef struct {
    /* ECDSA P-256 over `hash` with scalar d (BE). Used by SIG and AUT slots. */
    bool (*sign)(const uint8_t d[32],
                 const uint8_t *hash, uint16_t n,
                 uint8_t *out, uint16_t *out_n);
    /* UIF gate: 0 = not yet, 1 = authorized, 2 = denied/timeout. */
    int  (*confirm)(void);
    /* Derive the public key for `algo`.
     * PGP_ALGO_ECDSA_P256: writes 65 B (0x04||X||Y), *out_n = 65.
     * PGP_ALGO_ECDH (X25519): writes 32 B (RFC 7748 LE u), *out_n = 32.
     * NULL-tolerant: if NULL, READ PUBLIC KEY / GENERATE return 6A88. */
    bool (*pubkey)(uint8_t algo, const uint8_t d[32],
                   uint8_t *out, uint16_t *out_n);
    /* X25519 ECDH: shared secret = d · peer (peer = 32 B LE u-coordinate).
     * Writes 32 B, *out_n = 32. NULL-tolerant (PSO:DECIPHER → 6985). */
    bool (*ecdh)(const uint8_t d[32],
                 const uint8_t *peer, uint16_t peer_n,
                 uint8_t *out, uint16_t *out_n);
    /* Generate a private scalar for `algo` (Task 5). BE output.
     * NULL-tolerant (GENERATE 0x47/80 → 6985). */
    bool (*genkey)(uint8_t algo, uint8_t d_out[32]);
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
 * Returns true if all persist operations succeeded (key_persist + DO defaults).
 * Returns false if any NVS write failed; the in-RAM state is still coherent.
 * Safe to call at any time; used by tests and future admin commands.
 */
bool openpgp_card_factory_reset(void);

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
 * Returns true if a key is present in the Signature slot (imported via PUT
 * DATA 0xDB 3FFF or generated on-device).  Always reports the SIG slot — its
 * only current caller relies on that semantics.  False on a fresh /
 * factory-reset card.
 */
bool openpgp_card_key_is_set(void);

/*
 * Load persisted PIN/retry state and imported key from NVS into the applet
 * statics.  Must be called after openpgp_do_init() and before the card is
 * accessible to hosts (i.e. before any CCID traffic).  On a fresh device
 * (blobs absent) the call is a no-op and the factory baseline set by
 * openpgp_card_init() remains in effect.
 * On the test host this function is compiled as a no-op stub.
 */
void openpgp_card_load(void);

#ifdef TEST_HOST
/* Host-visible counter incremented by the pin_persist() stub.
 * Tests include this header to get the declaration; the definition lives in
 * openpgp_card.c under TEST_HOST. */
extern int g_pin_persist_calls;
#endif

/*
 * Set the crypto health flag.  Must be called by ccid_init() immediately after
 * openpgp_crypto_selftest().  When `ok` is false, PSO:CDS (INS 0x2A) and READ
 * PUBLIC KEY (INS 0x47 P1=0x81) return SW 0x6581 (memory failure) — the card
 * refuses all cryptographic operations on a broken device.  GET DATA, SELECT,
 * and VERIFY remain functional so that the host can still read card status to
 * diagnose the fault.
 * Default is true so host tests (which never call ccid_init) and the window
 * between openpgp_card_init() and ccid_init() both work correctly.
 * openpgp_card_init() and openpgp_card_factory_reset() do NOT reset this flag.
 */
void openpgp_card_set_crypto_health(bool ok);

/*
 * Process one command APDU (`in[0..in_len-1]`).
 * Writes the response (data + SW1 SW2) into `out[0..out_max-1]`.
 * Returns the number of bytes written (always >= 2 for the SW).
 */
uint16_t openpgp_card_apdu(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max);
