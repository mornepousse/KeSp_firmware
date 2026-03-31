/* Binary command handlers — Phase 3: system commands for testing */
#include "cdc_binary_cmds.h"
#include "cdc_internal.h"
#include "esp_app_desc.h"

/* ── System commands ────────────────────────────────────────────── */

static void bin_cmd_ping(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;
    ks_respond_ok(cmd_id);
}

static void bin_cmd_version(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver = desc->version;
    ks_respond(cmd_id, KS_STATUS_OK, (const uint8_t *)ver, (uint16_t)strlen(ver));
}

/* ── Command table ──────────────────────────────────────────────── */

static const ks_bin_cmd_entry_t bin_cmd_table[] = {
    { KS_CMD_PING,    bin_cmd_ping },
    { KS_CMD_VERSION, bin_cmd_version },
};

void cdc_binary_cmds_init(void)
{
    ks_register_binary_commands(bin_cmd_table, sizeof(bin_cmd_table) / sizeof(bin_cmd_table[0]));
}
