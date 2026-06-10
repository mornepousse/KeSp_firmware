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
