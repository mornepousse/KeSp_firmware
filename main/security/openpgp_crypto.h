/* main/security/openpgp_crypto.h — ECDSA P-256 signing for the OpenPGP applet (dongle). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Sign `hash` (length 20..64; mbedtls truncates per FIPS 186-4) with private
 * scalar d (32 B, big-endian). Output: raw r||s, exactly 64 bytes (NOT DER —
 * the OpenPGP card PSO:CDS format). Returns false on bad args / invalid
 * scalar / RNG or mbedtls error. */
bool openpgp_crypto_p256_sign(const uint8_t d[32],
                              const uint8_t *hash, uint16_t hash_len,
                              uint8_t sig[64]);

/* Sign-and-verify roundtrip with a fixed key+hash (RFC 6979 A.2.5 vector key,
 * SHA-256("sample")). Call once at boot; logs PASS/FAIL. Returns the verdict. */
bool openpgp_crypto_selftest(void);

/* Derive P-256 public point from private scalar d (32 B, big-endian).
 * Writes 0x04 || X(32) || Y(32) into out_pub (must hold 65 bytes).
 * Validates 0 < d < n.  Scrubs the scalar copy on exit.
 * Returns false on bad args, invalid scalar, or mbedtls error. */
bool openpgp_crypto_p256_pubkey(const uint8_t d[32], uint8_t out_pub[65]);
