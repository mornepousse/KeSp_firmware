/* main/security/apdu.h — ISO 7816-4 command-APDU parser (pure, host-testable) */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * Parsed command APDU.
 * `data` points into the original buffer — caller keeps it alive.
 * `le`   is the raw Le value; 0 in short form means 256 (ISO convention).
 */
typedef struct {
    uint8_t        cla;
    uint8_t        ins;
    uint8_t        p1;
    uint8_t        p2;
    uint16_t       lc;
    const uint8_t *data;
    uint32_t       le;
    bool           le_present;
} apdu_t;

/*
 * Parse `len` bytes from `buf` into `out`.
 * Returns true on success, false if the buffer is malformed or truncated.
 *
 * Handles ISO 7816-4 cases 1–4 (short) and extended length
 * (first Lc/Le byte == 0x00 with at least 7 total bytes).
 */
bool apdu_parse(const uint8_t *buf, uint16_t len, apdu_t *out);
