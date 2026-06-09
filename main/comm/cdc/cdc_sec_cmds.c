#include "cdc_sec_cmds.h"
#include <string.h>

bool sec_cmd_parse_set_slot(const uint8_t *p, uint16_t len,
                            uint8_t *idx, uint8_t *type,
                            char label[SEC_LABEL_LEN],
                            uint8_t secret[SEC_SECRET_MAX], uint8_t *secret_len)
{
    if (len < (uint16_t)(3 + SEC_LABEL_LEN)) return false;
    uint8_t i = p[0], t = p[1];
    uint8_t slen = p[2 + SEC_LABEL_LEN];
    if (i >= SEC_N_SLOTS) return false;
    if (slen > SEC_SECRET_MAX) return false;
    if (len < (uint16_t)(3 + SEC_LABEL_LEN + slen)) return false;
    *idx = i; *type = t; *secret_len = slen;
    memcpy(label, &p[2], SEC_LABEL_LEN);
    label[SEC_LABEL_LEN - 1] = '\0';
    if (slen) memcpy(secret, &p[3 + SEC_LABEL_LEN], slen);
    return true;
}

void sec_cmd_build_list(uint8_t *out, uint16_t *out_len)
{
    uint16_t n = 1;                          /* reserve count byte */
    uint8_t count = 0;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        if (sec_store_type(i) == SEC_SLOT_EMPTY) continue;
        out[n++] = i;
        out[n++] = sec_store_type(i);
        const char *lbl = sec_store_label(i);
        memset(&out[n], 0, SEC_LABEL_LEN);
        if (lbl) strncpy((char *)&out[n], lbl, SEC_LABEL_LEN - 1);
        n += SEC_LABEL_LEN;
        count++;
    }
    out[0] = count;
    *out_len = n;
}

#ifndef TEST_HOST
#include "cdc_binary_protocol.h"

static void bin_cmd_sec_set_slot(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];
    if (!sec_cmd_parse_set_slot(payload, len, &idx, &type, label, secret, &slen)) {
        ks_respond_err(cmd_id, KS_STATUS_ERR_INVALID);
        return;
    }
    if (!sec_store_set_slot(idx, type, label, secret, slen)) {
        ks_respond_err(cmd_id, KS_STATUS_ERR_INVALID);
        return;
    }
    ks_respond_ok(cmd_id);
}

static void bin_cmd_sec_clear_slot(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    if (len < 1 || !sec_store_clear_slot(payload[0])) {
        ks_respond_err(cmd_id, KS_STATUS_ERR_INVALID);
        return;
    }
    ks_respond_ok(cmd_id);
}

static void bin_cmd_sec_list(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;
    uint8_t buf[1 + SEC_N_SLOTS * (2 + SEC_LABEL_LEN)];
    uint16_t out_len = 0;
    sec_cmd_build_list(buf, &out_len);
    ks_respond(cmd_id, KS_STATUS_OK, buf, out_len);
}

static const ks_bin_cmd_entry_t sec_cmd_table[] = {
    { KS_CMD_SEC_SET_SLOT,   bin_cmd_sec_set_slot   },
    { KS_CMD_SEC_CLEAR_SLOT, bin_cmd_sec_clear_slot },
    { KS_CMD_SEC_LIST,       bin_cmd_sec_list       },
};

void cdc_sec_cmds_init(void)
{
    ks_register_binary_commands(sec_cmd_table, sizeof(sec_cmd_table) / sizeof(sec_cmd_table[0]));
}
#else
void cdc_sec_cmds_init(void) { /* host: not registered */ }
#endif
