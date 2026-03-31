/* Binary CDC protocol core — frame parsing, CRC-8, dispatch, response framing */
#include "cdc_binary_protocol.h"
#include "cdc_internal.h"

/* ── CRC-8/MAXIM (polynomial 0x31, init 0x00) ──────────────────── */

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

    tinyusb_cdcacm_write_queue(CDC_ITF, hdr, 6);
    if (len > 0 && payload)
        tinyusb_cdcacm_write_queue(CDC_ITF, payload, len);
    tinyusb_cdcacm_write_queue(CDC_ITF, &crc, 1);
    tinyusb_cdcacm_write_flush(CDC_ITF, 0);
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
    tinyusb_cdcacm_write_queue(CDC_ITF, hdr, 6);
    stream_crc = 0x00;
}

void ks_respond_write(const uint8_t *data, uint16_t len)
{
    tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
    /* Update CRC incrementally */
    for (uint16_t i = 0; i < len; i++) {
        stream_crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            stream_crc = (stream_crc & 0x80) ? (stream_crc << 1) ^ 0x31 : (stream_crc << 1);
    }
}

void ks_respond_end(void)
{
    tinyusb_cdcacm_write_queue(CDC_ITF, &stream_crc, 1);
    tinyusb_cdcacm_write_flush(CDC_ITF, 0);
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
    bool     ready;        /* complete frame waiting for dispatch */
} bin_rx;

void ks_rx_reset(void)
{
    bin_rx.state = KS_RX_IDLE;
    bin_rx.ready = false;
    bin_rx.hdr_pos = 0;
    bin_rx.payload_pos = 0;
}

uint16_t ks_rx_feed(const char *data, uint16_t len)
{
    uint16_t consumed = 0;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)data[i];

        switch (bin_rx.state) {
        case KS_RX_IDLE:
            if (b == KS_MAGIC_0) {
                bin_rx.state = KS_RX_MAGIC1;
                consumed = i + 1;
            } else {
                /* Not binary — return 0 to let caller handle as text */
                return 0;
            }
            break;

        case KS_RX_MAGIC1:
            if (b == KS_MAGIC_1) {
                bin_rx.state = KS_RX_HEADER;
                bin_rx.hdr_pos = 0;
                consumed = i + 1;
            } else {
                /* 'K' followed by non-'S' — not binary. Rewind. */
                bin_rx.state = KS_RX_IDLE;
                return 0;
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
                bin_rx.ready = true;
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

bool ks_process_one(void)
{
    if (!bin_rx.ready)
        return false;

    bin_rx.ready = false;
    uint8_t cmd_id = bin_rx.cmd_id;
    ks_bin_handler_t handler = find_handler(cmd_id);

    if (handler) {
        handler(cmd_id, bin_rx.payload, bin_rx.payload_len);
    } else {
        ESP_LOGW(TAG_CDC, "Unknown binary cmd 0x%02X", cmd_id);
        ks_respond_err(cmd_id, KS_STATUS_ERR_UNKNOWN);
    }
    return true;
}
