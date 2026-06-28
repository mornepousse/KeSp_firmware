/* Host-side tests for cfg_bridge — dongle-local vs. config-class routing predicate,
 * plus KS-frame chunk/reassemble logic for ESP-NOW tunnelling. */
#include "test_framework.h"
#include "../main/comm/rf/cfg_bridge.h"
#include "../main/comm/cdc/cdc_binary_protocol.h"
#include <string.h>

static void test_cfg_routing(void) {
    /* dongle-local: RF/pairing block */
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_RF_PAIR_START), "RF_PAIR_START local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_RF_STATUS),     "RF_STATUS local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_RF_PAIR_LIST),  "RF_PAIR_LIST local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_RF_PAIR_RESET), "RF_PAIR_RESET local");

    /* dongle-local: battery + monitoring */
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_BATTERY), "BATTERY local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_MONITOR), "MONITOR local");

    /* dongle-local: diagnostics that belong to the dongle */
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_NVS_RESET), "NVS_RESET local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_DFU),       "DFU local");

    /* dongle-local: security provisioning (dongle stores HMAC keys) */
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_SEC_SET_SLOT),  "SEC_SET_SLOT local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_SEC_CLEAR_SLOT),"SEC_CLEAR_SLOT local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_SEC_LIST),      "SEC_LIST local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_SEC_SELFTEST),  "SEC_SELFTEST local");

    /* dongle-local: OTA (flashes the dongle itself) */
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_OTA_START), "OTA_START local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_OTA_DATA),  "OTA_DATA local");
    TEST_ASSERT(cfg_is_dongle_local(KS_CMD_OTA_ABORT), "OTA_ABORT local");

    /* config-class → forwarded to the smart keyboard (keymap owner) */
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_SETKEY),      "SETKEY forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_SETLAYER),    "SETLAYER forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_MACRO_ADD),   "MACRO_ADD forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_COMBO_SET),   "COMBO_SET forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_LEADER_SET),  "LEADER_SET forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_TD_SET),      "TD_SET forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_KEYSTATS_BIN),"KEYSTATS_BIN forwarded");
    TEST_ASSERT(!cfg_is_dongle_local(KS_CMD_LAYER_NAME),  "LAYER_NAME forwarded");
}

static void test_cfg_chunking(void) {
    uint8_t frame[450];
    for (int i = 0; i < 450; i++) frame[i] = (uint8_t)(i * 7 + 1);

    uint8_t total = cfg_chunk_count(450);
    TEST_ASSERT_EQ(total, 3, "450 bytes -> 3 chunks of 200");
    TEST_ASSERT_EQ(cfg_chunk_count(0),   0, "empty -> 0");
    TEST_ASSERT_EQ(cfg_chunk_count(200), 1, "exactly 200 -> 1");
    TEST_ASSERT_EQ(cfg_chunk_count(201), 2, "201 -> 2");
    uint8_t max_chunks = (uint8_t)((CFG_FRAME_MAX + CFG_CHUNK_PAYLOAD - 1) / CFG_CHUNK_PAYLOAD);
    TEST_ASSERT_EQ(cfg_chunk_count(CFG_FRAME_MAX), max_chunks, "CFG_FRAME_MAX -> max chunks");
    TEST_ASSERT_EQ(cfg_chunk_count(CFG_FRAME_MAX + 1), 0, "over CFG_FRAME_MAX -> 0 (too large)");

    /* round-trip in order */
    cfg_reasm_t st;
    memset(&st, 0, sizeof(st));
    uint16_t outlen = 0;
    bool done = false;
    for (uint8_t i = 0; i < total; i++) {
        uint8_t c[CFG_CHUNK_HDR + CFG_CHUNK_PAYLOAD];
        uint16_t cl = cfg_chunk_make(frame, 450, i, c);
        TEST_ASSERT(cl > 0, "chunk made");
        done = cfg_reasm_add(&st, c, cl, &outlen);
    }
    TEST_ASSERT(done && outlen == 450 && memcmp(st.buf, frame, 450) == 0,
                "reassembled in order");

    /* round-trip out of order + duplicate tolerated */
    cfg_reasm_t st2;
    memset(&st2, 0, sizeof(st2));
    outlen = 0;
    uint8_t order[3] = {2, 0, 1};
    for (int k = 0; k < 3; k++) {
        uint8_t c[CFG_CHUNK_HDR + CFG_CHUNK_PAYLOAD];
        uint16_t cl = cfg_chunk_make(frame, 450, order[k], c);
        cfg_reasm_add(&st2, c, cl, &outlen);
        cfg_reasm_add(&st2, c, cl, &outlen); /* duplicate — must be idempotent */
    }
    /* All 3 distinct chunks were sent; buffer must be complete now */
    TEST_ASSERT(memcmp(st2.buf, frame, 450) == 0, "reassembled out-of-order");

    /* single small frame */
    uint8_t small[10] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    TEST_ASSERT_EQ(cfg_chunk_count(10), 1, "small -> 1 chunk");
    cfg_reasm_t st3;
    memset(&st3, 0, sizeof(st3));
    uint8_t c3[CFG_CHUNK_HDR + CFG_CHUNK_PAYLOAD];
    uint16_t cl3 = cfg_chunk_make(small, 10, 0, c3);
    TEST_ASSERT(cfg_reasm_add(&st3, c3, cl3, &outlen) && outlen == 10 &&
                memcmp(st3.buf, small, 10) == 0,
                "small roundtrip");

    /* bad-arg guards */
    TEST_ASSERT_EQ(cfg_chunk_make(NULL, 10, 0, c3), 0, "chunk_make NULL frame -> 0");
    TEST_ASSERT_EQ(cfg_chunk_make(small, 10, 5, c3), 0, "chunk_make idx>=total -> 0");
}

void test_cfg_bridge(void) {
    TEST_SUITE("cfg_bridge");
    test_cfg_routing();
    test_cfg_chunking();
}
