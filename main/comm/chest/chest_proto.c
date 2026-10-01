/* Niphar_chest link, S3 side — pure logic, protocol version 3. Contract:
 * Niphar_chest main/link/link_proto.{h,c} and test/test_link_proto.c at
 * cfd7b35, vectors V1-V16, R1-R3, L1, C1; spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md. */
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
    if (get_u16(&regs[CHEST_REG_CRC]) != cr_crc16(regs, CHEST_REG_CRC_SPAN)) return CHEST_BLOCK_CORRUPT;
    /* A length that overruns the field is a CORRUPT block, not one to
     * truncate: a master that truncated would read bytes that are not the
     * label and show a name the chest never composed (Review Focus, V6g). */
    if (regs[CHEST_REG_LABEL_LEN] > CHEST_LABEL_MAX) return CHEST_BLOCK_CORRUPT;
    uint8_t active = regs[CHEST_REG_MODE_ACTIVE];
    if ((regs[0x05] & CHEST_STATE_USB) && !(active >= CHEST_MODE_STORAGE && active <= CHEST_MODE_OATH))
        return CHEST_BLOCK_CORRUPT;                    /* mounted without a mode: KeSp's own invariant */
    if (out) {
        out->version = regs[0x04];
        out->state = regs[0x05];
        out->pending_op = get_u16(&regs[0x06]);
        out->confirm_count = get_u32(&regs[0x08]);
        out->instance = regs[CHEST_REG_INSTANCE];
        out->active_mode = active;
        out->op_count = regs[CHEST_REG_OP_COUNT];
        out->dma_kind = regs[CHEST_REG_DMA_KIND];
        out->dma_seq = regs[CHEST_REG_DMA_SEQ];
        out->dma_len = get_u16(&regs[CHEST_REG_DMA_LEN]);
        out->label_len = regs[CHEST_REG_LABEL_LEN];
        for (uint8_t i = 0; i < out->label_len; i++) {
            uint8_t c = regs[CHEST_REG_LABEL + i];
            out->label[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
        }
        out->label[out->label_len] = '\0';
    }
    return CHEST_BLOCK_OK;
}

bool chest_dma_segment_ok(const chest_status_t *st)
{
    if (!st) return false;
    if (st->dma_kind != CHEST_DMA_LIST && st->dma_kind != CHEST_DMA_CODE) return false;
    return st->dma_len >= 1 && st->dma_len <= CHEST_DMA_MAX;
}

void chest_op_label(uint16_t op, char out[CHEST_LABEL_BUF])
{
    /* The chest's sec_op_t codes (contract §1), pinned by value: the enum lives
     * in the other repository and new codes get appended. Codes 1-10: known labels;
     * 11-99: "OP nn"; >= 100: "OP ?" (6-char limit). */
    static const char *const k[] = { "", "SIGN", "DECRYP", "AUTH", "OTP", "FIDO +", "FIDO",
                                     "TOTP", "DELETE", "REPLAC", "RESET!" };
    if (op < sizeof k / sizeof k[0]) snprintf(out, CHEST_LABEL_BUF, "%s", k[op]);
    else if (op < 100) snprintf(out, CHEST_LABEL_BUF, "OP %u", (unsigned)op);
    else snprintf(out, CHEST_LABEL_BUF, "OP ?");
}

bool chest_press_matches(uint32_t pressed_tag, chest_block_t block, const chest_status_t *st)
{
    if (block != CHEST_BLOCK_OK || !st || pressed_tag == 0) return false;
    if (!(st->state & CHEST_STATE_READY)) return false;
    return CHEST_TAG_OP(pressed_tag) == st->pending_op && st->pending_op != 0
        && CHEST_TAG_INST(pressed_tag) == st->instance;
}

bool chest_confirm_request(chest_confirm_t *c, const chest_status_t *st, uint32_t now_ms)
{
    if (!st || st->pending_op == 0) return false;
    c->armed = true; c->writes = 0; c->op = st->pending_op; c->instance = st->instance;
    c->count0 = st->confirm_count; c->t_ms = now_ms;
    return true;
}

bool chest_confirm_step(chest_confirm_t *c, const chest_status_t *st, uint32_t now_ms)
{
    if (!c->armed || !st) return false;
    if (st->confirm_count != c->count0 || st->pending_op != c->op || st->instance != c->instance) {
        c->armed = false; return false;
    }
    if (c->writes == 0) { c->writes = 1; c->t_ms = now_ms; return true; }
    if ((uint32_t)(now_ms - c->t_ms) < CHEST_CONFIRM_RETRY_MS) return false;
    if (c->writes == 1) { c->writes = 2; c->t_ms = now_ms; return true; }
    c->armed = false;
    return false;
}

uint8_t chest_mode_next(uint8_t mode)
{
    return (mode < CHEST_MODE_OATH) ? (uint8_t)(mode + 1) : CHEST_MODE_NONE;
}

bool chest_mode_needs_write(const uint8_t *regs, uint8_t wanted)
{
    return regs && regs[CHEST_REG_MODE_REQ] != wanted;
}

chest_mode_state_t chest_mode_track(chest_mode_track_t *t, uint8_t active, uint8_t wanted)
{
    if (active == wanted) { t->differ_reads = 0; return CHEST_MODE_ARRIVED; }
    if (active == CHEST_MODE_IN_FLIGHT) { t->differ_reads = 0; return CHEST_MODE_PENDING; }
    if (t->differ_reads < CHEST_MODE_FAULT_READS) t->differ_reads++;
    return (t->differ_reads >= CHEST_MODE_FAULT_READS) ? CHEST_MODE_FAULT : CHEST_MODE_PENDING;
}

void chest_mode_label(chest_mode_state_t s, uint8_t active, uint8_t wanted, char out[CHEST_MODE_LABEL_BUF])
{
    /* Plain words (Mae, 2026-09-29): what the user does with the mode, not
     * the USB class — mass storage is DISK, OATH is TOTP. */
    static const char *const up[CHEST_MODE_COUNT] = { "", "DISK", "PGP", "OTP", "FIDO", "TOTP" };
    static const char *const lo[CHEST_MODE_COUNT] = { "", "disk", "pgp", "otp", "fido", "totp" };
    const char *txt = "";
    if (s == CHEST_MODE_FAULT) txt = "ERR";
    else if (s == CHEST_MODE_ARRIVED) txt = (active < CHEST_MODE_COUNT) ? up[active] : "";
    else txt = (wanted < CHEST_MODE_COUNT) ? lo[wanted] : "";
    snprintf(out, CHEST_MODE_LABEL_BUF, "%s", txt);
}

/* Cancelling a prompt — contract §5 (Niphar_chest 3da17cc). Same shape as
 * the confirm (tag at press, one write, one retry), but nothing counts a
 * cancel: delivery is read from the op/instance moving. */
void chest_cancel_pack(uint8_t out[2], uint8_t instance)
{
    out[0] = CHEST_CANCEL_MAGIC;
    out[1] = instance;
}

bool chest_cancel_request(chest_cancel_t *c, uint32_t tag, chest_block_t block,
                          const chest_status_t *st, uint32_t now_ms)
{
    if (!c || !st || block != CHEST_BLOCK_OK || tag == 0 || st->pending_op == 0) return false;
    if (CHEST_TAG_OP(tag) != st->pending_op || CHEST_TAG_INST(tag) != st->instance) return false;
    c->armed = true; c->writes = 0; c->op = st->pending_op; c->instance = st->instance; c->t_ms = now_ms;
    return true;
}

bool chest_cancel_step(chest_cancel_t *c, const chest_status_t *st, uint32_t now_ms)
{
    if (!c || !c->armed || !st) return false;
    if (st->pending_op != c->op || st->instance != c->instance) { c->armed = false; return false; }
    if (c->writes == 0) { c->writes = 1; c->t_ms = now_ms; return true; }
    if ((uint32_t)(now_ms - c->t_ms) < CHEST_CONFIRM_RETRY_MS) return false;
    if (c->writes == 1) { c->writes = 2; c->t_ms = now_ms; return true; }
    c->armed = false;
    return false;
}
