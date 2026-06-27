/* Host-side tests for cfg_bridge — dongle-local vs. config-class routing predicate. */
#include "test_framework.h"
#include "../main/comm/rf/cfg_bridge.h"
#include "../main/comm/cdc/cdc_binary_protocol.h"

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

void test_cfg_bridge(void) {
    TEST_SUITE("cfg_bridge");
    test_cfg_routing();
}
