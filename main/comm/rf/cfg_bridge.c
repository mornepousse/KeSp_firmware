/* cfg_bridge.c — dongle-local vs. config-class routing predicate, plus
 * KS-frame chunk/reassemble logic for ESP-NOW tunnelling.
 *
 * Pure C, no ESP-IDF includes. Safe to build host-side for tests.
 *
 * Dongle-local commands are handled by the dongle firmware directly.
 * Everything else is config-class: forwarded over the bridge to the paired
 * smart keyboard, which owns the keymap and feature configuration.
 */
#include "cfg_bridge.h"
#include "../cdc/cdc_binary_protocol.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * KS-frame chunk/reassemble — pure logic, no networking
 * ----------------------------------------------------------------------- */

uint8_t cfg_chunk_count(uint16_t frame_len)
{
    if (frame_len == 0 || frame_len > CFG_FRAME_MAX) return 0;
    return (uint8_t)((frame_len + CFG_CHUNK_PAYLOAD - 1) / CFG_CHUNK_PAYLOAD);
}

uint16_t cfg_chunk_make(const uint8_t *frame, uint16_t frame_len,
                        uint8_t idx, uint8_t *out)
{
    if (!frame || !out || frame_len == 0 || frame_len > CFG_FRAME_MAX) return 0;
    uint8_t total = cfg_chunk_count(frame_len);
    if (total == 0 || idx >= total) return 0;

    uint16_t offset    = (uint16_t)idx * CFG_CHUNK_PAYLOAD;
    uint16_t remaining = frame_len - offset;
    uint16_t slice     = (remaining > CFG_CHUNK_PAYLOAD) ? CFG_CHUNK_PAYLOAD : remaining;

    out[0] = idx;
    out[1] = total;
    memcpy(out + CFG_CHUNK_HDR, frame + offset, slice);
    return (uint16_t)(CFG_CHUNK_HDR + slice);
}

bool cfg_reasm_add(cfg_reasm_t *st, const uint8_t *chunk,
                   uint16_t chunk_len, uint16_t *out_len)
{
    if (!st || !chunk || !out_len) return false;
    /* Need at least header + 1 byte of data */
    if (chunk_len < (uint16_t)(CFG_CHUNK_HDR + 1)) return false;

    uint8_t  idx       = chunk[0];
    uint8_t  total     = chunk[1];
    uint16_t slice_len = chunk_len - CFG_CHUNK_HDR;

    /* Basic validity */
    if (total == 0 || idx >= total) return false;
    /* Reject oversized slice */
    if (slice_len > CFG_CHUNK_PAYLOAD) return false;
    /* Reject if inconsistent total across chunks */
    if (st->total != 0 && st->total != total) return false;

    uint16_t offset = (uint16_t)idx * CFG_CHUNK_PAYLOAD;
    /* Bounds: slice must not overflow buf */
    if ((uint32_t)offset + slice_len > CFG_FRAME_MAX) return false;

    /* Idempotent: ignore duplicates */
    uint8_t bit_mask = (uint8_t)(1u << (idx & 7u));
    if (st->seen[idx >> 3] & bit_mask) return false;

    /* Record total on first chunk */
    if (st->total == 0) st->total = total;

    /* Copy slice into reassembly buffer */
    memcpy(st->buf + offset, chunk + CFG_CHUNK_HDR, slice_len);

    /* Update high-water mark (last chunk determines true frame length) */
    uint16_t end = offset + slice_len;
    if (end > st->len) st->len = end;

    /* Mark this index as seen and count it */
    st->seen[idx >> 3] |= bit_mask;
    st->got++;

    if (st->got == st->total) {
        *out_len = st->len;
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * Dongle-local vs. config-class routing predicate
 * ----------------------------------------------------------------------- */

bool cfg_is_dongle_local(uint8_t cmd_id)
{
    switch (cmd_id) {
        /* System: DFU flashes the dongle itself */
        case KS_CMD_DFU:
        /* Diagnostics: RF link management */
        case KS_CMD_NVS_RESET:
        case KS_CMD_RF_PAIR_START:
        case KS_CMD_RF_STATUS:
        case KS_CMD_RF_PAIR_LIST:
        case KS_CMD_RF_PAIR_RESET:
        /* Diagnostics: dongle-reported battery + monitoring */
        case KS_CMD_BATTERY:
        case KS_CMD_MONITOR:
        /* Security: HMAC key slots live on the dongle */
        case KS_CMD_SEC_SET_SLOT:
        case KS_CMD_SEC_CLEAR_SLOT:
        case KS_CMD_SEC_LIST:
        case KS_CMD_SEC_SELFTEST:
        /* OTA: updates the dongle firmware */
        case KS_CMD_OTA_START:
        case KS_CMD_OTA_DATA:
        case KS_CMD_OTA_ABORT:
            return true;

        default:
            return false;
    }
}
