/* main/comm/cdc/cdc_sec_cmds.c — CDC provisioning for write-only secret slots (dongle). */
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
        uint8_t t = sec_store_type(i);
        if (t == SEC_SLOT_EMPTY) continue;
        out[n++] = i;
        out[n++] = t;
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
#include "cr_hmac.h"

static void bin_cmd_sec_set_slot(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    uint8_t idx, type, slen; char label[SEC_LABEL_LEN]; uint8_t secret[SEC_SECRET_MAX];
    bool parsed = sec_cmd_parse_set_slot(payload, len, &idx, &type, label, secret, &slen);
    bool stored = parsed && sec_store_set_slot(idx, type, label, secret, slen);
    memset(secret, 0, sizeof(secret));   /* scrub transient key material */
    if (!stored) {
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
    uint8_t buf[SEC_CMD_LIST_MAX];
    uint16_t out_len = 0;
    sec_cmd_build_list(buf, &out_len);
    ks_respond(cmd_id, KS_STATUS_OK, buf, out_len);
}

/* HMAC-SHA1 known-answer self-test (validates cr_hmac on real hardware without
 * needing a keypress). RFC 2202 case 1: key=0x0b x20, data="Hi There" ->
 * b617318655057264e28bc0b6fb378c8ef146be00. Returns the 20-byte digest. */
static void bin_cmd_sec_selftest(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;
    static const uint8_t key[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
    static const uint8_t msg[8] = { 'H','i',' ','T','h','e','r','e' };
    uint8_t out[20];
    if (cr_hmac_sha1(key, sizeof(key), msg, sizeof(msg), out))
        ks_respond(cmd_id, KS_STATUS_OK, out, sizeof(out));
    else
        ks_respond_err(cmd_id, KS_STATUS_ERR_INVALID);
}

static const ks_bin_cmd_entry_t sec_cmd_table[] = {
    { KS_CMD_SEC_SET_SLOT,   bin_cmd_sec_set_slot   },
    { KS_CMD_SEC_CLEAR_SLOT, bin_cmd_sec_clear_slot },
    { KS_CMD_SEC_LIST,       bin_cmd_sec_list       },
    { KS_CMD_SEC_SELFTEST,   bin_cmd_sec_selftest   },
};

void cdc_sec_cmds_init(void)
{
    ks_register_binary_commands(sec_cmd_table, sizeof(sec_cmd_table) / sizeof(sec_cmd_table[0]));
}
#else
void cdc_sec_cmds_init(void) { /* host: not registered */ }
#endif
