/*
 * test_cdc_rx_feed.c — CHARACTERIZATION tests for ks_rx_feed() and ks_crc8().
 *
 * Goal: pin down the CURRENT behavior of the binary KS frame parser
 * as it stands in cdc_binary_protocol.c. No bug fixes here.
 *
 * KS frame format (request):
 *   [0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]
 *
 * Required stubs (in test/):
 *   esp_log.h, tinyusb_cdc_acm.h, tusb.h, freertos/FreeRTOS.h, task.h
 * These stubs are resolved via the CMake include path (test/ is the first
 * directory), so they're transparent to cdc_binary_protocol.c.
 */
#include "test_framework.h"
#include "tinyusb_cdc_acm.h"               /* stub: esp_err_t, tinyusb_cdcacm_itf_t */
#include "../main/comm/cdc/cdc_binary_protocol.h"
#include <string.h>

/* ── Stubs required by cdc_binary_protocol.c at link time ─────────────────────── */

/* TAG_CDC declared extern in cdc_internal.h, defined here for this test TU. */
const char *TAG_CDC = "test_cdc";

/* CDC writes capture — reset before each sub-test. */
static uint8_t  fake_cdc_buf[256];
static size_t   fake_cdc_pos;
static int      fake_write_count;

esp_err_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf,
                                      const uint8_t *buf, size_t size)
{
    (void)itf;
    if (size > 0 && fake_cdc_pos + size <= sizeof(fake_cdc_buf))
        memcpy(fake_cdc_buf + fake_cdc_pos, buf, size);
    fake_cdc_pos += size;
    fake_write_count++;
    return 0;
}

esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf,
                                      uint32_t timeout_ticks)
{
    (void)itf; (void)timeout_ticks;
    return 0;
}

/* ── Test handler registered via ks_register_binary_commands ─────────── */

static bool     fake_rx_called;
static uint8_t  fake_rx_cmd;
static uint16_t fake_rx_len;
static uint8_t  fake_rx_payload[64]; /* enough for the test payloads */

static void test_cmd_handler(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    fake_rx_called = true;
    fake_rx_cmd    = cmd_id;
    fake_rx_len    = len;
    if (len > 0 && len <= sizeof(fake_rx_payload))
        memcpy(fake_rx_payload, payload, len);
}

static const ks_bin_cmd_entry_t test_cmd_table[] = {
    { KS_CMD_PING,    test_cmd_handler },
    { KS_CMD_VERSION, test_cmd_handler },
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Resets the parser state AND the capture state between each case. */
static void reset_state(void)
{
    ks_rx_reset();
    fake_cdc_pos      = 0;
    fake_write_count  = 0;
    memset(fake_cdc_buf, 0, sizeof(fake_cdc_buf));
    fake_rx_called    = false;
    fake_rx_cmd       = 0;
    fake_rx_len       = 0;
    memset(fake_rx_payload, 0, sizeof(fake_rx_payload));
}

/*
 * Builds a valid KS frame into buf.
 * - payload can be NULL if len==0 (ks_crc8 doesn't dereference with len=0).
 * - Returns the total size of the frame (6 + len).
 */
static size_t build_ks_frame(uint8_t *buf, uint8_t cmd,
                              const uint8_t *payload, uint16_t len)
{
    buf[0] = KS_MAGIC_0;
    buf[1] = KS_MAGIC_1;
    buf[2] = cmd;
    buf[3] = (uint8_t)(len & 0xFF);
    buf[4] = (uint8_t)((len >> 8) & 0xFF);
    if (len > 0 && payload)
        memcpy(buf + 5, payload, len);
    buf[5 + len] = ks_crc8(payload, len); /* NULL+0 → returns 0x00, safe */
    return (size_t)(6 + len);
}

/* ══ CRC-8 suite ═════════════════════════════════════════════════════════ */

/* Vector 1: empty payload → CRC = 0x00 (init only, loop doesn't run) */
static void test_crc8_empty_payload(void)
{
    TEST_ASSERT_EQ(ks_crc8(NULL, 0), 0x00,
                   "crc8(NULL,0) = 0x00 — init 0x00, loop skipped");
}

/* Vector 2: byte 0x00 → CRC = 0x00
 * (XOR then 8 shifts without triggering the polynomial: 0 stays 0) */
static void test_crc8_single_zero_byte(void)
{
    uint8_t d[] = {0x00};
    TEST_ASSERT_EQ(ks_crc8(d, 1), 0x00, "crc8([0x00]) = 0x00");
}

/* Vector 3: byte 0x01 → CRC = 0x31 (computed by hand, poly 0x31 MSB-first)
 * 0x01 → shift ×7 → 0x80 → MSB=1 → (0x80<<1)^0x31 = 0x00^0x31 = 0x31 */
static void test_crc8_single_nonzero_byte(void)
{
    uint8_t d[] = {0x01};
    TEST_ASSERT_EQ(ks_crc8(d, 1), 0x31, "crc8([0x01]) = 0x31");
}

/* Vector 4: multi-byte — backs the vector published in
 * docs/CDC_BINARY_PROTOCOL.md ([0x4B,0x53] → 0xBE) */
static void test_crc8_multibyte_doc_vector(void)
{
    uint8_t d[] = {0x4B, 0x53};
    TEST_ASSERT_EQ(ks_crc8(d, 2), 0xBE, "crc8([0x4B,0x53]) = 0xBE (doc)");
}

/* Vector 5: determinism — same input always gives the same output */
static void test_crc8_deterministic(void)
{
    uint8_t d[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQ(ks_crc8(d, 4), ks_crc8(d, 4),
                   "crc8 deterministic on the same data");
}

/* ══ ks_rx_feed parser suite ════════════════════════════════════════════ */

/* Case 1: valid frame passed all at once */
static void test_valid_frame_all_at_once(void)
{
    reset_state();
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);

    uint16_t consumed = ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT_EQ(consumed, (int)flen,
                   "whole frame: all bytes consumed");
    TEST_ASSERT(ks_process_one(),
                "ks_process_one returns true after a valid frame");
    TEST_ASSERT(fake_rx_called,       "handler called");
    TEST_ASSERT_EQ(fake_rx_cmd, KS_CMD_PING,  "cmd_id = PING");
    TEST_ASSERT_EQ(fake_rx_len, 0,    "payload_len = 0");
}

/* Case 2: same frame delivered byte by byte */
static void test_valid_frame_byte_by_byte(void)
{
    reset_state();
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);

    for (size_t i = 0; i < flen; i++) {
        uint16_t c = ks_rx_feed((const char *)(frame + i), 1);
        TEST_ASSERT_EQ(c, 1, "each individual byte is consumed (returns 1)");
    }
    TEST_ASSERT(ks_process_one(),
                "byte-by-byte frame: ks_process_one true");
    TEST_ASSERT(fake_rx_called, "handler called after byte-by-byte feed");
    TEST_ASSERT_EQ(fake_rx_cmd, KS_CMD_PING, "cmd_id correct after incremental feed");
}

/* Case 3: frame with non-empty payload */
static void test_frame_with_nonempty_payload(void)
{
    reset_state();
    uint8_t payload[] = {0xAB, 0xCD};
    uint8_t frame[32];
    size_t flen = build_ks_frame(frame, KS_CMD_VERSION,
                                 payload, sizeof(payload));

    uint16_t consumed = ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT_EQ(consumed, (int)flen,
                   "frame with payload: all bytes consumed");
    TEST_ASSERT(ks_process_one(), "frame with payload: ks_process_one true");
    TEST_ASSERT_EQ(fake_rx_len, 2, "payload_len = 2");
    TEST_ASSERT_EQ(fake_rx_payload[0], 0xAB, "payload[0] = 0xAB");
    TEST_ASSERT_EQ(fake_rx_payload[1], 0xCD, "payload[1] = 0xCD");
}

/* Case 4: noise before the magic → immediate return 0 (no advance in the stream) */
static void test_noise_before_magic_returns_zero(void)
{
    reset_state();

    /* In IDLE state, any byte ≠ 0x4B causes an immediate return 0.
     * The parser does NOT scan the buffer looking for the magic — it's up to the
     * caller to advance byte by byte. */
    uint8_t noise[] = {0xFF};
    uint16_t c = ks_rx_feed((const char *)noise, 1);
    TEST_ASSERT_EQ(c, 0,
                   "noise byte in IDLE: return 0 (no consumption)");

    /* After the noise, the state is still IDLE: a valid frame goes through. */
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);
    c = ks_rx_feed((const char *)frame, (uint16_t)flen);
    TEST_ASSERT_EQ(c, (int)flen,
                   "valid frame after noise: consumed normally");
    TEST_ASSERT(ks_process_one(),
                "valid frame after noise: ks_process_one true");
}

/* Case 5: magic1 correct (0x4B) but magic2 incorrect → return 0, state reset to IDLE
 *
 * Actual behavior: the function has already set consumed=1 (for the 0x4B),
 * but returns 0 (not consumed). The caller interprets this as "zero bytes
 * consumed in binary" and treats 0x4B as text. */
static void test_bad_magic2_returns_zero_and_resets(void)
{
    reset_state();

    /* 0x4B=magic0 correct, 0x52=KR_MAGIC_1 ≠ KS_MAGIC_1 */
    uint8_t bad[] = {KS_MAGIC_0, KR_MAGIC_1};
    uint16_t c = ks_rx_feed((const char *)bad, 2);
    TEST_ASSERT_EQ(c, 0,
                   "invalid magic2: return 0 (not consumed)");

    /* The state has gone back to IDLE: a valid frame follows without issue. */
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);
    c = ks_rx_feed((const char *)frame, (uint16_t)flen);
    TEST_ASSERT_EQ(c, (int)flen, "frame after bad magic2: consumed");
    TEST_ASSERT(ks_process_one(),  "frame after bad magic2: ok");
}

/* Case 6: incorrect CRC → frame rejected, KR ERR_CRC response sent */
static void test_bad_crc_frame_rejected(void)
{
    reset_state();
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);
    /* Corrupts the last byte (the CRC) */
    frame[flen - 1] ^= 0xFF;

    uint16_t consumed = ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT_EQ(consumed, (int)flen,
                   "bad-CRC frame: all bytes are still consumed");
    TEST_ASSERT(!ks_process_one(),
                "ks_process_one returns false (frame rejected, ready=false)");

    /* A KR error response must have been sent over CDC. */
    TEST_ASSERT(fake_cdc_pos > 0, "KR response sent on the CDC output");
    TEST_ASSERT_EQ(fake_cdc_buf[0], KS_MAGIC_0,     "KR response: magic0=0x4B");
    TEST_ASSERT_EQ(fake_cdc_buf[1], KR_MAGIC_1,     "KR response: magic1=0x52 (R)");
    TEST_ASSERT_EQ(fake_cdc_buf[2], KS_CMD_PING,    "KR response: cmd_id=PING");
    TEST_ASSERT_EQ(fake_cdc_buf[3], KS_STATUS_ERR_CRC,
                   "KR response: status=ERR_CRC (0x02)");
}

/* Case 7: correct CRC → frame accepted (complement of case 6) */
static void test_correct_crc_frame_accepted(void)
{
    reset_state();
    uint8_t payload[] = {0xCA, 0xFE};
    uint8_t frame[32];
    size_t flen = build_ks_frame(frame, KS_CMD_VERSION,
                                 payload, sizeof(payload));

    ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT(ks_process_one(),  "correct CRC: ks_process_one true");
    TEST_ASSERT(fake_rx_called,    "correct CRC: handler called");
    TEST_ASSERT_EQ(fake_rx_len, 2, "correct CRC: payload_len = 2");
    /* No error response should have been sent. */
    TEST_ASSERT_EQ(fake_cdc_pos, 0,
                   "correct CRC: no error response sent");
}

/* Case 8: payload_len > KS_PAYLOAD_MAX → immediate rejection, ERR_OVERFLOW response
 *
 * Actual behavior:
 *  - ks_respond_err(cmd, ERR_OVERFLOW) is called as soon as the header ends
 *  - The state resets to IDLE (the "payload" bytes in the buffer keep
 *    being read by the loop, but in IDLE state every byte ≠ 0x4B
 *    would cause a premature return 0 — see note in the report)
 *  - Here we do NOT send payload bytes after the header to avoid
 *    the ambiguity (see behavior noted in the report).
 */
static void test_oversized_payload_rejected(void)
{
    reset_state();

    /* payload_len = 0x1001 = 4097 > KS_PAYLOAD_MAX (4096) */
    uint8_t frame[] = {
        KS_MAGIC_0, KS_MAGIC_1,
        KS_CMD_PING,
        0x01, 0x10  /* len_lo=0x01, len_hi=0x10 → 0x1001 = 4097 */
    };

    uint16_t consumed = ks_rx_feed((const char *)frame, sizeof(frame));

    /* All header bytes (5) are consumed. */
    TEST_ASSERT_EQ(consumed, 5,
                   "overflow: the 5 header bytes are consumed");

    /* ERR_OVERFLOW error response sent. */
    TEST_ASSERT(fake_cdc_pos > 0,
                "overflow: KR response sent");
    TEST_ASSERT_EQ(fake_cdc_buf[0], KS_MAGIC_0,          "overflow: magic0");
    TEST_ASSERT_EQ(fake_cdc_buf[3], KS_STATUS_ERR_OVERFLOW,
                   "overflow: status=ERR_OVERFLOW (0x06)");

    /* No frame ready. */
    TEST_ASSERT(!ks_process_one(),
                "overflow: ks_process_one false (no frame ready)");
}

/* Case 9: overflow + following non-magic byte → consumed = header (not 0)
 *
 * Regression: after an overflow the state goes back to IDLE; if a
 * non-magic byte follows in the same buffer, the parser must reflect the bytes
 * already consumed (the header), not return 0 (which would make the
 * caller believe nothing was consumed while the error was actually sent). */
static void test_oversized_then_garbage_consumes_header(void)
{
    reset_state();
    uint8_t buf[] = {
        KS_MAGIC_0, KS_MAGIC_1, KS_CMD_PING,
        0x01, 0x10,   /* len = 0x1001 = 4097 > KS_PAYLOAD_MAX */
        0xFF          /* non-magic byte after the overflow */
    };
    uint16_t c = ks_rx_feed((const char *)buf, sizeof(buf));
    TEST_ASSERT_EQ(c, 5,
                   "overflow + non-magic byte: consumed = 5 (header), not 0");
}

/* Case 10: overflow + garbage byte + valid frame → the frame is NOT lost
 *
 * Regression: a premature return 0 in IDLE after the overflow used to stop the
 * scan and cause loss of a valid frame located further in the buffer. */
static void test_oversized_then_garbage_then_valid_frame(void)
{
    reset_state();
    uint8_t valid[16];
    size_t vlen = build_ks_frame(valid, KS_CMD_PING, NULL, 0);

    uint8_t buf[6 + 16];
    buf[0] = KS_MAGIC_0; buf[1] = KS_MAGIC_1; buf[2] = KS_CMD_VERSION;
    buf[3] = 0x01; buf[4] = 0x10;   /* header oversized */
    buf[5] = 0xFF;                  /* garbage byte */
    memcpy(buf + 6, valid, vlen);

    ks_rx_feed((const char *)buf, (uint16_t)(6 + vlen));
    TEST_ASSERT(ks_process_one(),
                "valid frame after overflow+garbage: parsed (not lost)");
    TEST_ASSERT(fake_rx_called,
                "handler called for the frame that follows the overflow");
    TEST_ASSERT_EQ(fake_rx_cmd, KS_CMD_PING, "cmd of the rescued frame = PING");
}

/* ══ Suite entry point ════════════════════════════════════════ */

void test_cdc_rx_feed(void)
{
    printf("\n--- cdc_rx_feed / ks_crc8 ---\n");

    /*
     * The command tables are static in cdc_binary_protocol.c.
     * We register once for the whole suite — ks_rx_reset()
     * does not touch these tables.
     */
    ks_register_binary_commands(
        test_cmd_table,
        sizeof(test_cmd_table) / sizeof(test_cmd_table[0]));

    /* CRC-8 vectors */
    TEST_RUN(test_crc8_empty_payload);
    TEST_RUN(test_crc8_single_zero_byte);
    TEST_RUN(test_crc8_single_nonzero_byte);
    TEST_RUN(test_crc8_multibyte_doc_vector);
    TEST_RUN(test_crc8_deterministic);

    /* Parser ks_rx_feed */
    TEST_RUN(test_valid_frame_all_at_once);
    TEST_RUN(test_valid_frame_byte_by_byte);
    TEST_RUN(test_frame_with_nonempty_payload);
    TEST_RUN(test_noise_before_magic_returns_zero);
    TEST_RUN(test_bad_magic2_returns_zero_and_resets);
    TEST_RUN(test_bad_crc_frame_rejected);
    TEST_RUN(test_correct_crc_frame_accepted);
    TEST_RUN(test_oversized_payload_rejected);
    TEST_RUN(test_oversized_then_garbage_consumes_header);
    TEST_RUN(test_oversized_then_garbage_then_valid_frame);
}
