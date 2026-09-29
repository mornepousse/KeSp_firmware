/* Niphar_chest link, S3 side — pure logic. Contract: Niphar_chest
 * docs/LINK_CONTRACT.md; spec: docs/superpowers/specs/2026-09-29-chest-link-s3-master-design.md. */
#include "chest_proto.h"
#include <stdio.h>
#include <string.h>
#include "cr_crc16.h"

static const uint8_t k_magic[4] = { 'N', 'I', 'P', 'H' };
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool chest_proto_is_absent(const uint8_t *regs, size_t len)
{
    if (!regs || len == 0) return true;
    if (regs[0] != 0x00 && regs[0] != 0xFF) return false;
    for (size_t i = 1; i < len; i++) if (regs[i] != regs[0]) return false;
    return true;
}

chest_block_t chest_proto_parse(const uint8_t *regs, size_t len, chest_status_t *out)
{
    if (!regs || len < CHEST_REG_SIZE) return CHEST_BLOCK_CORRUPT;
    if (chest_proto_is_absent(regs, CHEST_REG_SIZE)) return CHEST_BLOCK_ABSENT;
    if (memcmp(regs, k_magic, 4) != 0) return CHEST_BLOCK_CORRUPT;
    if (regs[0x04] != CHEST_PROTO_VERSION) return CHEST_BLOCK_BAD_VERSION;
    if (get_u16(&regs[0x0C]) != cr_crc16(regs, 0x0C)) return CHEST_BLOCK_CORRUPT;
    if (out) {
        out->version = regs[0x04];
        out->state = regs[0x05];
        out->pending_op = get_u16(&regs[0x06]);
        out->confirm_count = get_u32(&regs[0x08]);
    }
    return CHEST_BLOCK_OK;
}

void chest_op_label(uint16_t op, char out[CHEST_LABEL_BUF])
{
    /* The chest's sec_op_t codes (contract §1), pinned by value: the enum lives
     * in the other repository and new codes get appended. */
    static const char *const k[] = { "", "SIGN", "DECRYP", "AUTH", "OTP", "FIDO +", "FIDO",
                                     "TOTP", "DELETE", "REPLAC", "RESET!" };
    if (op < sizeof k / sizeof k[0]) snprintf(out, CHEST_LABEL_BUF, "%s", k[op]);
    else snprintf(out, CHEST_LABEL_BUF, "OP %u", (unsigned)(op % 1000u));
}

bool chest_confirm_request(chest_confirm_t *c, uint16_t pending_op, uint32_t count, uint32_t now_ms)
{
    if (pending_op == 0) return false;
    c->armed = true; c->writes = 0; c->op = pending_op; c->count0 = count; c->t_ms = now_ms;
    return true;
}

bool chest_confirm_step(chest_confirm_t *c, uint16_t pending_op, uint32_t count, uint32_t now_ms)
{
    if (!c->armed) return false;
    if (count != c->count0 || pending_op != c->op) { c->armed = false; return false; }
    if (c->writes == 0) { c->writes = 1; c->t_ms = now_ms; return true; }
    if ((uint32_t)(now_ms - c->t_ms) < CHEST_CONFIRM_RETRY_MS) return false;
    if (c->writes == 1) { c->writes = 2; c->t_ms = now_ms; return true; }
    c->armed = false;
    return false;
}
