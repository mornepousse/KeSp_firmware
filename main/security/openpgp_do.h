/* main/security/openpgp_do.h — OpenPGP Data-Object store (pure, host-testable) */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Maximum payload bytes stored per DO.
 * 255 is the hard ceiling imposed by tlv_append() (which rejects n > 255);
 * a stored DO of 256 bytes would later break GET DATA 6E with 6F00. */
#define OPENPGP_DO_MAX_LEN     255
/* Maximum number of distinct DOs held simultaneously.
 * Factory defaults occupy 17 distinct tags (4F, 5F52, C0-C3, C5, C6, CD,
 * D6-D8, 5B, 5F2D, 5F35, 5F50, 93). 24 leaves slack for gpg PUT DATA
 * of C1/C2/C3 overwrites and user DOs.  RAM cost: 24 × 261 B ≈ 6.3 KB. */
#define OPENPGP_DO_MAX_ENTRIES  24

/*
 * Store the value `v[0..n-1]` under tag `tag` (uint16_t, big-endian P1:P2
 * convention: single-byte tags are 0x00XX).
 * Returns false if n > OPENPGP_DO_MAX_LEN or the table is full.
 * Overwrites an existing entry with the same tag.
 */
bool openpgp_do_put(uint16_t tag, const uint8_t *v, uint16_t n);

/*
 * Retrieve the stored value for `tag`.
 * On success sets *v to an internal pointer (valid until next put/reset)
 * and *n to its length.  Returns false if the tag has not been set.
 */
bool openpgp_do_get(uint16_t tag, const uint8_t **v, uint16_t *n);

/* Wipe all stored DOs (called from openpgp_card_factory_reset and on test reset). */
void openpgp_do_reset(void);
