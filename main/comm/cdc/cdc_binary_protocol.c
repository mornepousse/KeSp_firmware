/* Binary CDC protocol core — frame parsing, CRC-8, dispatch, response framing */
#include "cdc_binary_protocol.h"
#include "cdc_internal.h"
#include "cfg_bridge.h"   /* CFG_FRAME_MAX, cfg_is_dongle_local, bridge fns */
#include <string.h>

/* ── RX handoff lock (audit E5) ──────────────────────────────────────
 * ks_rx_feed() (tâche callback USB) et ks_process_one() (tâche cdc_cmd)
 * tournent sur des tâches différentes. Le buffer d'assemblage était aussi le
 * buffer de dispatch → un hôte qui pipeline (n'attend pas la réponse) pouvait
 * faire dispatcher un payload en cours de réécriture (jamais re-CRC comme un
 * tout). On copie la frame validée dans ready_frame sous mutex ; feed ne la
 * réécrit pas tant qu'elle n'est pas consommée. */
#ifndef TEST_HOST
#include "freertos/semphr.h"
static SemaphoreHandle_t s_rx_mutex = NULL;
static inline void ks_rx_lock(void)   { if (s_rx_mutex) xSemaphoreTake(s_rx_mutex, portMAX_DELAY); }
static inline void ks_rx_unlock(void) { if (s_rx_mutex) xSemaphoreGive(s_rx_mutex); }
void ks_rx_init(void)                 { if (!s_rx_mutex) s_rx_mutex = xSemaphoreCreateMutex(); }
#else
static inline void ks_rx_lock(void)   {}
static inline void ks_rx_unlock(void) {}
void ks_rx_init(void)                 {}
#endif

/* ── Response redirect (ESP-NOW config bridge) ──────────────────────
 * When set (only on a smart keyboard handling an ESP-NOW-arrived KS frame), KS
 * responses accumulate into s_resp_redirect_buf instead of USB CDC, and are sent
 * back over ESP-NOW on flush. Default false → USB path is byte-identical. */
static bool     s_resp_redirect;
static uint8_t  s_resp_redirect_buf[CFG_FRAME_MAX];
static uint16_t s_resp_redirect_len;

/* Append bytes to the redirect buffer, bounds-checked vs CFG_FRAME_MAX.
 * Silently drops the overflow tail (the bridge caps forwarded responses). */
static void resp_redirect_append(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return;
    uint16_t room = (s_resp_redirect_len < CFG_FRAME_MAX)
                  ? (uint16_t)(CFG_FRAME_MAX - s_resp_redirect_len) : 0;
    uint16_t n = (len < room) ? len : room;
    if (n) {
        memcpy(s_resp_redirect_buf + s_resp_redirect_len, data, n);
        s_resp_redirect_len = (uint16_t)(s_resp_redirect_len + n);
    }
}

void ks_resp_redirect_begin(void)
{
    s_resp_redirect     = true;
    s_resp_redirect_len = 0;
}

const uint8_t *ks_resp_redirect_end(uint16_t *len)
{
    s_resp_redirect = false;
    if (len) *len = s_resp_redirect_len;
    return s_resp_redirect_buf;
}

/* ── CRC-8: polynomial 0x31, init 0x00, MSB-first, no reflection ──
 * NOT the catalogued CRC-8/MAXIM (which is reflected). See
 * docs/CDC_BINARY_PROTOCOL.md for the reference impl + test vectors. */

uint8_t ks_crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

/* ── Binary command table registry ──────────────────────────────── */

static struct {
    const ks_bin_cmd_entry_t *table;
    size_t count;
} bin_tables[KS_MAX_BIN_CMD_TABLES];
static size_t bin_tables_count = 0;

void ks_register_binary_commands(const ks_bin_cmd_entry_t *table, size_t count)
{
    if (bin_tables_count < KS_MAX_BIN_CMD_TABLES) {
        bin_tables[bin_tables_count].table = table;
        bin_tables[bin_tables_count].count = count;
        bin_tables_count++;
        ESP_LOGI(TAG_CDC, "Registered %u binary commands", (unsigned)count);
    }
}

static ks_bin_handler_t find_handler(uint8_t cmd_id)
{
    for (size_t t = 0; t < bin_tables_count; t++) {
        const ks_bin_cmd_entry_t *tbl = bin_tables[t].table;
        for (size_t i = 0; i < bin_tables[t].count; i++) {
            if (tbl[i].cmd_id == cmd_id)
                return tbl[i].handler;
        }
    }
    return NULL;
}

/* ── Response framing ───────────────────────────────────────────── */

void ks_respond(uint8_t cmd_id, uint8_t status, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[6] = {
        KS_MAGIC_0, KR_MAGIC_1,
        cmd_id, status,
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF)
    };
    uint8_t crc = ks_crc8(payload, len);

    if (s_resp_redirect) {
        resp_redirect_append(hdr, 6);
        if (len > 0 && payload)
            resp_redirect_append(payload, len);
        resp_redirect_append(&crc, 1);
    } else {
        tinyusb_cdcacm_write_queue(CDC_ITF, hdr, 6);
        if (len > 0 && payload)
            tinyusb_cdcacm_write_queue(CDC_ITF, payload, len);
        tinyusb_cdcacm_write_queue(CDC_ITF, &crc, 1);
        tinyusb_cdcacm_write_flush(CDC_ITF, 0);
    }
}

void ks_respond_ok(uint8_t cmd_id)
{
    ks_respond(cmd_id, KS_STATUS_OK, NULL, 0);
}

void ks_respond_err(uint8_t cmd_id, uint8_t status)
{
    ks_respond(cmd_id, status, NULL, 0);
}

/* ── Streaming response ─────────────────────────────────────────── */

static uint8_t stream_crc;

void ks_respond_begin(uint8_t cmd_id, uint8_t status, uint16_t total_len)
{
    uint8_t hdr[6] = {
        KS_MAGIC_0, KR_MAGIC_1,
        cmd_id, status,
        (uint8_t)(total_len & 0xFF), (uint8_t)((total_len >> 8) & 0xFF)
    };
    if (s_resp_redirect) {
        resp_redirect_append(hdr, 6);
    } else {
        tinyusb_cdcacm_write_queue(CDC_ITF, hdr, 6);
    }
    stream_crc = 0x00;
}

void ks_respond_write(const uint8_t *data, uint16_t len)
{
    if (s_resp_redirect) {
        resp_redirect_append(data, len);
    } else {
        tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
    }
    /* Update CRC incrementally */
    for (uint16_t i = 0; i < len; i++) {
        stream_crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            stream_crc = (stream_crc & 0x80) ? (stream_crc << 1) ^ 0x31 : (stream_crc << 1);
    }
}

void ks_respond_end(void)
{
    if (s_resp_redirect) {
        resp_redirect_append(&stream_crc, 1);
    } else {
        tinyusb_cdcacm_write_queue(CDC_ITF, &stream_crc, 1);
        tinyusb_cdcacm_write_flush(CDC_ITF, 0);
    }
}

/* ── Receive-side state machine ─────────────────────────────────── */

/* Single-entry binary command buffer (host waits for response before next) */
static struct {
    ks_rx_state_t state;
    uint8_t  cmd_id;
    uint16_t payload_len;
    uint16_t payload_pos;
    uint8_t  hdr_buf[3];   /* cmd_id, len_lo, len_hi */
    uint8_t  hdr_pos;
    uint8_t  payload[KS_PAYLOAD_MAX];
} bin_rx;

/* Frame validée en attente de dispatch — copiée depuis bin_rx sous ks_rx_lock().
 * Découple l'assemblage (bin_rx, écrit uniquement par feed) du dispatch. */
static struct {
    uint8_t  cmd_id;
    uint16_t payload_len;
    uint8_t  payload[KS_PAYLOAD_MAX];
    bool     ready;
} ready_frame;

void ks_rx_reset(void)
{
    bin_rx.state = KS_RX_IDLE;
    bin_rx.hdr_pos = 0;
    bin_rx.payload_pos = 0;
    ks_rx_lock();
    ready_frame.ready = false;
    ks_rx_unlock();
}

uint16_t ks_rx_feed(const char *data, uint16_t len)
{
    uint16_t consumed = 0;
    /* True once a frame header has been committed in THIS call (magic1
     * confirmed — stays set even when an oversized header resets state to
     * IDLE). Distinguishes "buffer starts with non-binary → text" (return 0)
     * from "stray byte after a frame/overflow → keep scanning" so a later
     * valid frame in the same buffer is not lost. */
    bool progressed = false;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)data[i];

        switch (bin_rx.state) {
        case KS_RX_IDLE:
            if (b == KS_MAGIC_0) {
                bin_rx.state = KS_RX_MAGIC1;
                consumed = i + 1;
            } else if (!progressed) {
                /* Buffer doesn't start with a frame — let caller treat as text */
                return 0;
            }
            /* else: stray byte after a committed frame/overflow — skip it and
             * keep scanning for the next magic in this buffer */
            break;

        case KS_RX_MAGIC1:
            if (b == KS_MAGIC_1) {
                bin_rx.state = KS_RX_HEADER;
                bin_rx.hdr_pos = 0;
                consumed = i + 1;
                progressed = true;
            } else {
                bin_rx.state = KS_RX_IDLE;
                if (!progressed)
                    return 0;  /* 'K' not followed by 'S' at buffer start → text */
                /* else: false magic mid-stream (e.g. after an overflow reset)
                 *       — skip this byte, keep scanning for the next frame */
            }
            break;

        case KS_RX_HEADER:
            bin_rx.hdr_buf[bin_rx.hdr_pos++] = b;
            consumed = i + 1;
            if (bin_rx.hdr_pos == 3) {
                bin_rx.cmd_id = bin_rx.hdr_buf[0];
                bin_rx.payload_len = (uint16_t)bin_rx.hdr_buf[1]
                                   | ((uint16_t)bin_rx.hdr_buf[2] << 8);
                bin_rx.payload_pos = 0;
                if (bin_rx.payload_len > KS_PAYLOAD_MAX) {
                    /* Payload too large — reject */
                    ks_respond_err(bin_rx.cmd_id, KS_STATUS_ERR_OVERFLOW);
                    bin_rx.state = KS_RX_IDLE;
                } else if (bin_rx.payload_len == 0) {
                    bin_rx.state = KS_RX_CRC;
                } else {
                    bin_rx.state = KS_RX_PAYLOAD;
                }
            }
            break;

        case KS_RX_PAYLOAD:
            bin_rx.payload[bin_rx.payload_pos++] = b;
            consumed = i + 1;
            if (bin_rx.payload_pos >= bin_rx.payload_len)
                bin_rx.state = KS_RX_CRC;
            break;

        case KS_RX_CRC: {
            consumed = i + 1;
            uint8_t expected = ks_crc8(bin_rx.payload, bin_rx.payload_len);
            if (b == expected) {
                /* Copie atomique de la frame validée pour le dispatcher. */
                ks_rx_lock();
                if (!ready_frame.ready) {
                    ready_frame.cmd_id      = bin_rx.cmd_id;
                    ready_frame.payload_len = bin_rx.payload_len;
                    if (bin_rx.payload_len)
                        memcpy(ready_frame.payload, bin_rx.payload, bin_rx.payload_len);
                    ready_frame.ready = true;
                } else {
                    ESP_LOGW(TAG_CDC, "BIN frame dropped (dispatcher busy) cmd=0x%02X",
                             bin_rx.cmd_id);
                }
                ks_rx_unlock();
            } else {
                ESP_LOGW(TAG_CDC, "BIN CRC mismatch cmd=0x%02X (got 0x%02X, exp 0x%02X)",
                         bin_rx.cmd_id, b, expected);
                ks_respond_err(bin_rx.cmd_id, KS_STATUS_ERR_CRC);
            }
            bin_rx.state = KS_RX_IDLE;
            /* Stop consuming — remaining bytes belong to the next frame or text */
            return consumed;
        }
        }
    }
    return consumed;
}

void ks_dispatch_frame(const uint8_t *frame, uint16_t len)
{
    /* frame = [0x4B][0x53][cmd][len_lo][len_hi][payload...][crc8] */
    if (!frame || len < 6) return;
    if (frame[0] != KS_MAGIC_0 || frame[1] != KS_MAGIC_1) return;

    uint8_t  cmd_id = frame[2];
    uint16_t plen   = (uint16_t)frame[3] | ((uint16_t)frame[4] << 8);
    /* Exact length: magic(2)+cmd(1)+len(2)+payload+crc(1) */
    if ((uint32_t)plen + 6 != (uint32_t)len) return;

    const uint8_t *payload = frame + 5;
    uint8_t crc = frame[5 + plen];
    if (ks_crc8(payload, plen) != crc) {
        ks_respond_err(cmd_id, KS_STATUS_ERR_CRC);
        return;
    }

    ks_bin_handler_t handler = find_handler(cmd_id);
    if (handler) {
        handler(cmd_id, payload, plen);
    } else {
        ks_respond_err(cmd_id, KS_STATUS_ERR_UNKNOWN);
    }
}

bool ks_process_one(void)
{
    ks_rx_lock();
    bool have = ready_frame.ready;
    ks_rx_unlock();
    if (!have)
        return false;

    /* Dispatch depuis ready_frame : feed ne le réécrit pas tant que ready est
     * vrai, donc payload reste stable pendant toute la commande (audit E5). */
    uint8_t        cmd_id      = ready_frame.cmd_id;
    uint16_t       payload_len = ready_frame.payload_len;
    const uint8_t *payload     = ready_frame.payload;

#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    /* Config bridge: config-class commands are owned by the paired smart
     * keyboard. Reconstruct the raw KS frame and tunnel it over ESP-NOW; the
     * KR returns asynchronously via EN_KR_CHUNK and is written out USB CDC. */
    if (!cfg_is_dongle_local(cmd_id) && cfg_bridge_have_smart_kbd()) {
        uint16_t full_len = (uint16_t)(6 + payload_len);
        if (full_len <= CFG_FRAME_MAX) {
            /* static, not on the stack: CFG_FRAME_MAX is 6KB and the CDC dispatch
             * task stack would overflow (reboot). ks_process_one is the single,
             * non-reentrant CDC dispatcher, so a shared static buffer is safe. */
            static uint8_t frame[CFG_FRAME_MAX];
            frame[0] = KS_MAGIC_0;
            frame[1] = KS_MAGIC_1;
            frame[2] = cmd_id;
            frame[3] = (uint8_t)(payload_len & 0xFF);
            frame[4] = (uint8_t)((payload_len >> 8) & 0xFF);
            if (payload_len)
                memcpy(frame + 5, payload, payload_len);
            frame[5 + payload_len] = ks_crc8(payload, payload_len);
            cfg_bridge_forward_frame(frame, full_len);
        } else {
            ks_respond_err(cmd_id, KS_STATUS_ERR_OVERFLOW);
        }
        ks_rx_lock(); ready_frame.ready = false; ks_rx_unlock();
        return true;
    }
#endif

    ks_bin_handler_t handler = find_handler(cmd_id);

    if (handler) {
        handler(cmd_id, payload, payload_len);
    } else {
        ESP_LOGW(TAG_CDC, "Unknown binary cmd 0x%02X", cmd_id);
        ks_respond_err(cmd_id, KS_STATUS_ERR_UNKNOWN);
    }

    ks_rx_lock(); ready_frame.ready = false; ks_rx_unlock();
    return true;
}
