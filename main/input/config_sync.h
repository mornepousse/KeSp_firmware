#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Config coherence between the two keymap engines (dongle + left) — fusion.
 *
 * Two engines must not type differently. Rather than a hand-kept version
 * number (initial design, rule 2), we take a FINGERPRINT of the content:
 * a CRC-32 of the config blob. Same content -> same fingerprint; any
 * difference changes it. The controller reads the fingerprint of both
 * devices (equal = synchronized); the dongle will compare the fingerprint
 * announced by the left to its own to refuse to run on a divergence (guardrail to come).
 *
 * Pure logic, tested on host (test/test_config_sync.c).
 * Design: docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 */

/* IEEE CRC-32 (reflected, poly 0xEDB88320) — the standard "zlib/gzip" CRC-32.
 * Bitwise: no table, a few KB of config to hash once, the cost is
 * negligible and the code stays identical on host and target. */
static inline uint32_t config_fp_crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Do two fingerprints designate the same config? Equal AND non-zero: 0 means
 * "unknown / not yet announced", never an agreement. */
static inline bool config_fp_match(uint32_t a, uint32_t b)
{
    return a != 0u && a == b;
}

/* -- Coherence state on the dongle side (fusion) ----------------------------
 *
 * The dongle retains the last fingerprint announced by the left (via the
 * config_fp field of PKT_TYPE_STATUS) and exposes it to the controller over
 * CDC. It only logs/acts on CHANGE: the left announces on every status frame
 * (~1/s), and reacting to each one would flood the console — the same
 * discipline as the link's "emit on change". The `vue` flag distinguishes
 * "never announced" from "announced 0", which left_fp alone cannot separate.
 *
 * Pure logic, tested on host (test/test_config_sync.c). */
typedef struct {
    uint32_t left_fp;   /* last fingerprint announced by the left */
    uint32_t left_ms;   /* when (ms), for the age exposed to the controller */
    bool     vue;       /* false = no announcement received yet */
} config_coherence_t;

/* The left has just announced `fp` at instant `now_ms`. Remembers, and returns
 * true if it is a CHANGE (new fingerprint, or first announcement). */
static inline bool config_coherence_note(config_coherence_t *c, uint32_t fp,
                                         uint32_t now_ms)
{
    bool change = (!c->vue) || (fp != c->left_fp);
    c->left_fp = fp;
    c->left_ms = now_ms;
    c->vue     = true;
    return change;
}
