/* Binary CDC protocol — KaSe keyboard firmware.
 *
 * Frame format:
 *   Request:  [0x4B][0x53][cmd_id:u8][len:u16 LE][payload...][crc8]
 *   Response: [0x4B][0x52][cmd_id:u8][status:u8][len:u16 LE][payload...][crc8]
 *
 * Backward compat: if first byte != 0x4B, fall back to legacy ASCII text.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Frame magic bytes ──────────────────────────────────────────── */

#define KS_MAGIC_0  0x4B  /* 'K' */
#define KS_MAGIC_1  0x53  /* 'S' — request */
#define KR_MAGIC_1  0x52  /* 'R' — response */

/* ── Max payload size (must fit OTA chunks) ─────────────────────── */

#define KS_PAYLOAD_MAX  4096

/* ── Command IDs ────────────────────────────────────────────────── */

typedef enum {
    /* System (0x01-0x0F) */
    KS_CMD_VERSION          = 0x01,
    KS_CMD_FEATURES         = 0x02,
    KS_CMD_DFU              = 0x03,
    KS_CMD_PING             = 0x04,

    /* Keymap (0x10-0x1F) */
    KS_CMD_SETLAYER         = 0x10,
    KS_CMD_SETKEY           = 0x11,
    KS_CMD_KEYMAP_CURRENT   = 0x12,
    KS_CMD_KEYMAP_GET       = 0x13,
    KS_CMD_LAYER_INDEX      = 0x14,
    KS_CMD_LAYER_NAME       = 0x15,

    /* Layout names (0x20-0x2F) */
    KS_CMD_SET_LAYOUT_NAME  = 0x20,
    KS_CMD_LIST_LAYOUTS     = 0x21,
    KS_CMD_GET_LAYOUT_JSON  = 0x22,

    /* Macros (0x30-0x3F) */
    KS_CMD_LIST_MACROS      = 0x30,
    KS_CMD_MACRO_ADD        = 0x31,
    KS_CMD_MACRO_ADD_SEQ    = 0x32,
    KS_CMD_MACRO_DELETE     = 0x33,

    /* Stats (0x40-0x4F) */
    KS_CMD_KEYSTATS_BIN     = 0x40,
    KS_CMD_KEYSTATS_TEXT    = 0x41,
    KS_CMD_KEYSTATS_RESET   = 0x42,
    KS_CMD_BIGRAMS_BIN      = 0x43,
    KS_CMD_BIGRAMS_TEXT     = 0x44,
    KS_CMD_BIGRAMS_RESET    = 0x45,

    /* Tap Dance (0x50-0x5F) */
    KS_CMD_TD_SET           = 0x50,
    KS_CMD_TD_LIST          = 0x51,

    /* Combos (0x60-0x6F) */
    KS_CMD_COMBO_SET        = 0x60,
    KS_CMD_COMBO_LIST       = 0x61,

    /* Leader (0x70-0x7F) */
    KS_CMD_LEADER_SET       = 0x70,
    KS_CMD_LEADER_LIST      = 0x71,

    /* Bluetooth (0x80-0x8F) */
    KS_CMD_BT_QUERY         = 0x80,
    KS_CMD_BT_SWITCH        = 0x81,
    KS_CMD_BT_PAIR          = 0x82,
    KS_CMD_BT_DISCONNECT    = 0x83,
    KS_CMD_BT_NEXT          = 0x84,
    KS_CMD_BT_PREV          = 0x85,

    /* Features (0x90-0x9F) */
    KS_CMD_AUTOSHIFT_TOGGLE = 0x90,
    KS_CMD_KO_SET           = 0x91,
    KS_CMD_KO_LIST          = 0x92,
    KS_CMD_WPM_QUERY        = 0x93,
    KS_CMD_TRILAYER_SET     = 0x94,

    /* Tamagotchi (0xA0-0xAF) */
    KS_CMD_TAMA_QUERY       = 0xA0,
    KS_CMD_TAMA_ENABLE      = 0xA1,
    KS_CMD_TAMA_DISABLE     = 0xA2,
    KS_CMD_TAMA_FEED        = 0xA3,
    KS_CMD_TAMA_PLAY        = 0xA4,
    KS_CMD_TAMA_SLEEP       = 0xA5,
    KS_CMD_TAMA_MEDICINE    = 0xA6,
    KS_CMD_TAMA_SAVE        = 0xA7,

    /* OTA (0xF0-0xFF) */
    KS_CMD_OTA_START        = 0xF0,
    KS_CMD_OTA_DATA         = 0xF1,
    KS_CMD_OTA_ABORT        = 0xF2,
} ks_cmd_id_t;

/* ── Status codes ───────────────────────────────────────────────── */

typedef enum {
    KS_STATUS_OK            = 0x00,
    KS_STATUS_ERR_UNKNOWN   = 0x01,
    KS_STATUS_ERR_CRC       = 0x02,
    KS_STATUS_ERR_INVALID   = 0x03,
    KS_STATUS_ERR_RANGE     = 0x04,
    KS_STATUS_ERR_BUSY      = 0x05,
    KS_STATUS_ERR_OVERFLOW  = 0x06,
} ks_status_t;

/* ── Binary command handler ─────────────────────────────────────── */

typedef void (*ks_bin_handler_t)(uint8_t cmd_id, const uint8_t *payload, uint16_t len);

typedef struct {
    uint8_t          cmd_id;
    ks_bin_handler_t handler;
} ks_bin_cmd_entry_t;

#define KS_MAX_BIN_CMD_TABLES 4
void ks_register_binary_commands(const ks_bin_cmd_entry_t *table, size_t count);

/* ── Response helpers ───────────────────────────────────────────── */

void ks_respond(uint8_t cmd_id, uint8_t status, const uint8_t *payload, uint16_t len);
void ks_respond_ok(uint8_t cmd_id);
void ks_respond_err(uint8_t cmd_id, uint8_t status);

/* Streaming response: for large payloads without full buffering.
 * Computes CRC incrementally as data is written. */
void ks_respond_begin(uint8_t cmd_id, uint8_t status, uint16_t total_len);
void ks_respond_write(const uint8_t *data, uint16_t len);
void ks_respond_end(void);

/* ── CRC-8 ──────────────────────────────────────────────────────── */

uint8_t ks_crc8(const uint8_t *data, uint16_t len);

/* ── Receive-side frame parser ──────────────────────────────────── */

typedef enum {
    KS_RX_IDLE,
    KS_RX_MAGIC1,
    KS_RX_HEADER,
    KS_RX_PAYLOAD,
    KS_RX_CRC,
} ks_rx_state_t;

/* Feed raw bytes into the binary parser.
 * Returns the number of bytes consumed. Unconsumed bytes should be
 * passed to the legacy text assembler. */
uint16_t ks_rx_feed(const char *data, uint16_t len);

/* Reset parser state (e.g. on timeout or error) */
void ks_rx_reset(void);

/* Process a fully assembled binary command from the FIFO.
 * Called from the CDC processing task. Returns true if a command was processed. */
bool ks_process_one(void);
