/* Niphar_chest link, DMA channel — pure logic. Contract: Niphar_chest
 * main/link/link_proto.{h,c} and test/test_link_proto.c at commit 440d79d
 * (vectors identical to cfd7b35), vectors R1-R3, L1, C1; spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §3. */
#include "chest_dma.h"
#include <string.h>
#include "cr_crc16.h"

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static char sanitize(uint8_t c) { return (c >= 0x20 && c <= 0x7E) ? (char)c : '?'; }

void chest_req_pack(uint8_t out[CHEST_REQ_SIZE], uint8_t cmd, uint8_t arg)
{
    if (!out) return;
    memset(out, 0x00, CHEST_REQ_SIZE);
    out[0] = cmd;
    out[1] = arg;
    put_u16(&out[CHEST_REQ_SIZE - 2], cr_crc16(out, CHEST_REQ_SIZE - 2));
}

bool chest_list_decode(const uint8_t *buf, uint16_t len, chest_list_t *out)
{
    if (!buf || !out) return false;
    if (len < CHEST_LIST_HDR_SIZE + 2u) return false;
    if (len > CHEST_DMA_MAX) return false;
    if (get_u16(&buf[len - 2]) != cr_crc16(buf, (size_t)(len - 2))) return false;

    uint8_t count = buf[CHEST_LIST_OFF_COUNT];
    if (count > CHEST_LIST_MAX_ENTRIES) return false;

    chest_list_t tmp;
    tmp.total = buf[CHEST_LIST_OFF_TOTAL];
    tmp.count = count;
    tmp.first = buf[CHEST_LIST_OFF_FIRST];
    tmp.more = (buf[CHEST_LIST_OFF_FLAGS] & CHEST_LIST_FLAG_MORE) != 0;

    uint16_t o = CHEST_LIST_HDR_SIZE;
    const uint16_t end = (uint16_t)(len - 2u);
    for (uint8_t i = 0; i < count; i++) {
        /* Redundant with the "name runs past the CRC" check just below: once
         * name_off (== o + 2) itself exceeds `end`, name_off + namelen > end
         * too for any namelen >= 0, so that check alone already refuses this
         * case. Kept anyway as a local, self-contained proof at the point
         * where o is used as an offset — an equivalent mutant, not a gap: a
         * mutation campaign that flags its removal as "surviving" should read
         * this comment before spending a test vector on it. */
        if ((uint16_t)(o + 2u) > end) return false;   /* index + length bytes run past the CRC */
        uint8_t idx = buf[o];
        uint8_t namelen = buf[o + 1];
        if (namelen > CHEST_LABEL_MAX) return false;
        uint16_t name_off = (uint16_t)(o + 2u);
        if ((uint16_t)(name_off + namelen) > end) return false;   /* name runs past the CRC */

        tmp.e[i].index = idx;
        for (uint8_t j = 0; j < namelen; j++)
            tmp.e[i].name[j] = sanitize(buf[name_off + j]);
        tmp.e[i].name[namelen] = '\0';

        o = (uint16_t)(name_off + namelen);
    }
    if (o != end) return false;   /* bytes left over between the last entry and the CRC */

    *out = tmp;
    return true;
}

bool chest_code_decode(const uint8_t *buf, uint16_t len, chest_code_t *out)
{
    if (!buf || !out) return false;
    if (len != CHEST_CODE_SIZE) return false;
    if (get_u16(&buf[CHEST_CODE_SIZE - 2]) != cr_crc16(buf, CHEST_CODE_SIZE - 2)) return false;

    uint8_t digits = buf[1];
    if (digits != 6 && digits != 8) return false;

    for (uint8_t i = 0; i < 8; i++)
        if (buf[2 + i] < '0' || buf[2 + i] > '9') return false;

    /* link_proto_pack_code (Niphar_chest main/link/link_proto.c at 440d79d)
     * memsets the whole 8-byte field to '0' BEFORE writing the code
     * right-justified: for digits == 6, the two leftmost bytes are the
     * padding the packer itself always produces, never the code. A 6-digit
     * answer whose padding is not exactly "00" is not a code the chest ever
     * packed — desync or corruption, not a real code. Only checked for
     * digits == 6: an 8-digit code fills the whole field, no padding byte
     * to check. */
    if (digits == 6 && (buf[2] != '0' || buf[3] != '0')) return false;

    /* The chest's own sec_time_window_remaining() (main/security/sec_time.c
     * at 440d79d, SEC_TIME_TOTP_STEP == 30) returns 1..30: 0 is impossible
     * for a real code (it would mean an already-expired window) and > 30 is
     * impossible for the same step. */
    uint8_t seconds = buf[10];
    if (seconds == 0 || seconds > 30) return false;

    chest_code_t tmp;
    tmp.index = buf[0];
    tmp.digits = digits;
    tmp.seconds = seconds;
    /* The LAST `digits` characters of the 8-byte, zero-padded-left field. */
    memcpy(tmp.code, &buf[2 + (8u - digits)], digits);
    tmp.code[digits] = '\0';

    *out = tmp;
    return true;
}
