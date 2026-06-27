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

/* -------------------------------------------------------------------------
 * ESP-NOW networking bridge — firmware only (see cfg_bridge.h).
 * ----------------------------------------------------------------------- */
#ifndef TEST_HOST

#if CONFIG_KASE_DEVICE_ROLE_DONGLE || CONFIG_KASE_KBD_WIRELESS
#include "espnow_link.h"        /* espnow_send() */
#include "espnow_msg.h"         /* EN_KS_CHUNK / EN_KR_CHUNK */
#include "esp_log.h"

static const char *TAG_CFG = "cfg_bridge";

/* Chunk `frame` (len bytes) and send each chunk with the given ESP-NOW type. */
static void cfg_bridge_send_chunked(const uint8_t mac[6], uint8_t en_type,
                                    const uint8_t *frame, uint16_t len)
{
    uint8_t total = cfg_chunk_count(len);
    if (total == 0) {
        ESP_LOGW(TAG_CFG, "send: frame_len %u out of range (drop)", len);
        return;
    }
    uint8_t chunk[CFG_CHUNK_HDR + CFG_CHUNK_PAYLOAD];
    for (uint8_t idx = 0; idx < total; idx++) {
        uint16_t clen = cfg_chunk_make(frame, len, idx, chunk);
        if (clen == 0) {
            ESP_LOGW(TAG_CFG, "send: chunk %u build failed (abort)", idx);
            return;
        }
        /* espnow_send prepends the type byte; payload is [idx][total][slice]. */
        espnow_send(mac, en_type, chunk, clen);
    }
}
#endif /* DONGLE || KBD_WIRELESS */

#if CONFIG_KASE_DEVICE_ROLE_DONGLE
#include "rf_rx_task.h"         /* rf_rx_copy_peer_macs() */
#include "cdc_acm_com.h"        /* cdc_send_binary() */
#include <string.h>

/* Resolve the smart keyboard's MAC.
 * TODO(HW): slot/device-type per-pairing storage is not wired yet, so we target
 * the LEFT paired slot as the smart keyboard. Finalize device-type selection at
 * HW integration time (a smart V2D keyboard pairs into one slot). */
static bool cfg_bridge_smart_kbd_mac(uint8_t out_mac[6])
{
    uint8_t mac_left[6]  = {0};
    uint8_t mac_right[6] = {0};
    rf_rx_copy_peer_macs(mac_left, mac_right);
    bool has_left = mac_left[0] | mac_left[1] | mac_left[2] |
                    mac_left[3] | mac_left[4] | mac_left[5];
    if (!has_left) return false;
    memcpy(out_mac, mac_left, 6);
    return true;
}

bool cfg_bridge_have_smart_kbd(void)
{
    uint8_t mac[6];
    return cfg_bridge_smart_kbd_mac(mac);
}

void cfg_bridge_forward_frame(const uint8_t *frame, uint16_t len)
{
    uint8_t mac[6];
    if (!cfg_bridge_smart_kbd_mac(mac)) {
        ESP_LOGW(TAG_CFG, "forward: no smart keyboard paired (drop)");
        return;
    }
    cfg_bridge_send_chunked(mac, EN_KS_CHUNK, frame, len);
}

void cfg_bridge_recv_kr_chunk(const uint8_t mac[6],
                              const uint8_t *chunk, uint16_t chunk_len)
{
    (void)mac;
    static cfg_reasm_t s_kr_reasm;   /* dongle: reassemble KR from keyboard */
    uint16_t out_len = 0;
    if (cfg_reasm_add(&s_kr_reasm, chunk, chunk_len, &out_len)) {
        /* Full KR frame reassembled — write it out USB CDC. */
        cdc_send_binary(s_kr_reasm.buf, out_len);
        memset(&s_kr_reasm, 0, sizeof(s_kr_reasm));
    }
}
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

#if CONFIG_KASE_KBD_WIRELESS
#include "cdc_binary_protocol.h"   /* ks_dispatch_frame, ks_resp_redirect_* */
#include <string.h>

void cfg_bridge_handle_ks_frame(const uint8_t mac[6],
                                const uint8_t *frame, uint16_t len)
{
    /* Dispatch with the KR response captured into the redirect buffer, then
     * chunk it back to the dongle over ESP-NOW. */
    ks_resp_redirect_begin();
    ks_dispatch_frame(frame, len);
    uint16_t resp_len = 0;
    const uint8_t *resp = ks_resp_redirect_end(&resp_len);
    if (resp_len > 0) {
        cfg_bridge_send_chunked(mac, EN_KR_CHUNK, resp, resp_len);
    }
}

void cfg_bridge_recv_ks_chunk(const uint8_t mac[6],
                              const uint8_t *chunk, uint16_t chunk_len)
{
    static cfg_reasm_t s_ks_reasm;   /* keyboard: reassemble KS from dongle */
    uint16_t out_len = 0;
    if (cfg_reasm_add(&s_ks_reasm, chunk, chunk_len, &out_len)) {
        cfg_bridge_handle_ks_frame(mac, s_ks_reasm.buf, out_len);
        memset(&s_ks_reasm, 0, sizeof(s_ks_reasm));
    }
}
#endif /* CONFIG_KASE_KBD_WIRELESS */

#endif /* !TEST_HOST */

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
