/* cfg_bridge.c — dongle-local vs. config-class routing predicate.
 *
 * Pure C, no ESP-IDF includes. Safe to build host-side for tests.
 *
 * Dongle-local commands are handled by the dongle firmware directly.
 * Everything else is config-class: forwarded over the bridge to the paired
 * smart keyboard, which owns the keymap and feature configuration.
 */
#include "cfg_bridge.h"
#include "../cdc/cdc_binary_protocol.h"

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
