/* main/security/openpgp_card.c — OpenPGP applet state machine */
#include "openpgp_card.h"
#include "openpgp_do.h"
#include "apdu.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* OpenPGP application AID prefix (first 6 bytes) */
static const uint8_t OPGP_AID_PREFIX[6] = {0xD2,0x76,0x00,0x01,0x24,0x01};

/* Factory-default PINs */
static const uint8_t PW1_DEFAULT[6]  = {'1','2','3','4','5','6'};
static const uint8_t PW3_DEFAULT[8]  = {'1','2','3','4','5','6','7','8'};
#define PW1_DEFAULT_LEN  6
#define PW3_DEFAULT_LEN  8
#define PW1_RETRY_MAX    3
#define PW3_RETRY_MAX    3
#define PW_MAX_LEN      16
#define FP_SLICE_LEN    20u  /* fingerprint slice length (C7/C8/C9 PUT DATA) */
#define TS_SLICE_LEN     4u  /* timestamp slice length (CE/CF/D0 PUT DATA)    */
/* PGP_ALGO_ECDSA_P256 / PGP_ALGO_ECDH are declared in openpgp_card.h */
#define PGP_P256_SCALAR_BYTES 32u    /* private scalar size in bytes (P-256) */

/* Status words */
#define SW_OK              0x9000u
#define SW_SEC_NOT_SAT     0x6982u  /* security status not satisfied */
#define SW_COND_NOT_SAT    0x6985u  /* conditions of use not satisfied */
#define SW_AUTH_BLOCKED    0x6983u  /* authentication method blocked */
#define SW_REF_NOT_FOUND   0x6A88u  /* referenced data not found */
#define SW_INS_NOT_SUP     0x6D00u  /* instruction not supported */
#define SW_WRONG_P1P2      0x6A86u  /* incorrect parameters P1-P2 */
#define SW_WRONG_DATA      0x6A80u  /* incorrect data field */

/* ------------------------------------------------------------------ */
/* Factory-default Data Object values (OpenPGP 3.4)                   */
/* ------------------------------------------------------------------ */

/* AID 0x4F — 16 bytes.  Serial bytes [10..13] patched from efuse MAC on
 * target (Task 4); left as 00 00 00 00 for host / first-boot. */
static const uint8_t FACTORY_AID[16] = {
    0xD2, 0x76, 0x00, 0x01, 0x24, 0x01,   /* RID + OpenPGP application 01 */
    0x03, 0x04,                            /* version 3.4                  */
    0xFF, 0x00,                            /* manufacturer 0xFF00 (test)   */
    0x00, 0x00, 0x00, 0x00,               /* serial (host default)        */
    0x00, 0x00                             /* RFU                          */
};

/* Historical bytes 0x5F52 — mirrors the ATR historical bytes */
static const uint8_t FACTORY_HIST[10] = {
    0x00, 0x31, 0x84, 0x73, 0x80, 0x01, 0x80, 0x00, 0x90, 0x00
};

/* Extended capabilities 0xC0 — 10 bytes:
 * byte0 0x34 = key-import(0x20)|PW-status-changeable(0x10)|algo-attrs-changeable(0x04)
 * bytes 6-7 = max special-DO length 0x0100 */
static const uint8_t FACTORY_C0[10] = {
    0x34, 0x00,  0x00, 0x00,  0x00, 0x00,  0x01, 0x00,  0x00, 0x00
};

/* Algorithm attributes: 0x13=ECDSA (PGP_ALGO_ECDSA_P256), 0x12=ECDH; OID = NIST P-256 */
static const uint8_t FACTORY_C1[9] = {
    0x13, /* PGP_ALGO_ECDSA_P256 */ 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};
/* Algorithm attributes 0xC2 — ECDH with cv25519 (OID 1.3.6.1.4.1.3029.1.5.1),
 * gpg's default ENC subkey algorithm since 2.3. */
static const uint8_t FACTORY_C2[11] = {
    0x12, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x97, 0x55, 0x01, 0x05, 0x01
};
static const uint8_t FACTORY_C3[9] = {
    0x13, /* PGP_ALGO_ECDSA_P256 */ 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};

/* UIF DOs: byte0 0x01=required, byte1 0x20=button-present.
 * Signing ON by default; decryption and authentication off. */
static const uint8_t FACTORY_D6[2] = {0x01, 0x20};
static const uint8_t FACTORY_D7[2] = {0x00, 0x20};
static const uint8_t FACTORY_D8[2] = {0x00, 0x20};

/* General Feature Management 0x7F74 — OpenPGP 3.4 §4.4.3.
 * Template: tag 0x81 (compact TLV), len 1, value 0x20 = button/keypad present.
 * scdaemon queries this DO directly and sets extcap.has_button from it. */
static const uint8_t FACTORY_7F74[3] = {0x81, 0x01, 0x20};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static const openpgp_card_hooks_t *s_hooks;

static bool    s_selected;
static bool    s_terminated;         /* after INS 0xE6 — only ACTIVATE revives */
static bool    s_pw1_sign_verified;  /* mode 0x81 — required for PSO:CDS */
static bool    s_pw1_user_verified;  /* mode 0x82 — gate for PSO:DECIPHER (Phase 2) */
static bool    s_pw3_verified;
static uint8_t s_pw1_retry;          /* shared counter for both PW1 modes */
static uint8_t s_pw3_retry;

/* Crypto KAT health flag.  Set externally by ccid_init() after
 * openpgp_crypto_selftest().  Default true: host tests never call the setter,
 * and the boot window between openpgp_card_init() and ccid_init() must not
 * leave the card hard-blocked.  openpgp_card_init/factory_reset do NOT touch
 * this flag — it is owned by the ccid layer. */
static bool s_crypto_ok = true;

/* Mutable PINs (changeable via CHANGE REFERENCE DATA — Phase 2) */
static uint8_t s_pw1[PW_MAX_LEN];
static uint8_t s_pw1_len;
static uint8_t s_pw3[PW_MAX_LEN];
static uint8_t s_pw3_len;

/* Private keys, one per OpenPGP slot.  d is big-endian as received from gpg.
 * origin per DO 0xDE encoding: 1 = generated on-device, 2 = imported. */
typedef struct {
    uint8_t set;
    uint8_t algo;     /* PGP_ALGO_ECDSA_P256 / PGP_ALGO_ECDH */
    uint8_t origin;
    uint8_t d[32];
} pgp_key_t;
static pgp_key_t s_keys[OPENPGP_SLOT_COUNT];

/* NVS blob layout for PIN/retry persistence ("pgp_pins"). */
typedef struct {
    uint8_t pw1[PW_MAX_LEN];
    uint8_t pw1_len;
    uint8_t pw1_retry;
    uint8_t pw3[PW_MAX_LEN];
    uint8_t pw3_len;
    uint8_t pw3_retry;
} pgp_pins_blob_t;

static bool key_persist(void); /* NVS on target, no-op on host; defined below */
static bool pin_persist(void); /* NVS on target, counter on host; defined below */

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Write SW + optional data into out[]; return total response length. */
static uint16_t respond(uint8_t *out, uint16_t out_max,
                        const uint8_t *data, uint16_t data_len,
                        uint16_t sw)
{
    if (out_max < 2) return 0;
    if (data_len + 2u > out_max)
        data_len = (uint16_t)(out_max - 2);
    if (data && data_len)
        memcpy(out, data, data_len);
    out[data_len]     = (uint8_t)(sw >> 8);
    out[data_len + 1] = (uint8_t)(sw & 0xFF);
    return (uint16_t)(data_len + 2);
}

static uint16_t sw_only(uint8_t *out, uint16_t out_max, uint16_t sw)
{
    return respond(out, out_max, NULL, 0, sw);
}

/* BER-TLV appender — single-byte or two-byte tags, short/long-form length.
 * Supports n = 0..255.  Returns new offset, or 0 on overflow / n>255. */
static uint16_t tlv_append(uint8_t *buf, uint16_t max, uint16_t off,
                            uint16_t tag, const uint8_t *v, uint16_t n)
{
    if (n > 255u) return 0;  /* would need 0x82 encoding — not needed here */

    uint16_t tag_bytes = (tag > 0xFFu) ? 2u : 1u;
    uint16_t len_bytes = (n >= 128u)   ? 2u : 1u;
    uint32_t needed    = (uint32_t)tag_bytes + len_bytes + n;

    if ((uint32_t)off + needed > (uint32_t)max) return 0;

    if (tag > 0xFFu) {
        buf[off++] = (uint8_t)(tag >> 8);
        buf[off++] = (uint8_t)(tag & 0xFFu);
    } else {
        buf[off++] = (uint8_t)tag;
    }

    if (n >= 128u) {
        buf[off++] = 0x81u;
        buf[off++] = (uint8_t)n;
    } else {
        buf[off++] = (uint8_t)n;
    }

    if (n > 0u) {
        if (v) memcpy(buf + off, v, n);
        else   memset(buf + off, 0, n);
    }
    return (uint16_t)(off + n);
}

/* Restore factory PINs and retry counters to RAM state.
 * Called from openpgp_card_init() (boot baseline) and
 * openpgp_card_factory_reset() (full wipe). */
static void pins_factory(void)
{
    memcpy(s_pw1, PW1_DEFAULT, PW1_DEFAULT_LEN);
    s_pw1_len   = PW1_DEFAULT_LEN;
    memcpy(s_pw3, PW3_DEFAULT, PW3_DEFAULT_LEN);
    s_pw3_len   = PW3_DEFAULT_LEN;
    s_pw1_retry = PW1_RETRY_MAX;
    s_pw3_retry = PW3_RETRY_MAX;
}

/* Fill a 7-byte PW status buffer (DO 0xC4) from live retry counters. */
static void fill_pw_status(uint8_t c4[7])
{
    const uint8_t *c4v; uint16_t c4n;
    /* PW1 validity: byte0 of the stored C4 DO (0 = valid for one PSO:CDS,
     * 1 = valid for several / gpg "forcesig off"); default 0 if absent. */
    c4[0] = (openpgp_do_get(0x00C4u, &c4v, &c4n) && c4n >= 1) ? c4v[0] : 0x00;
    c4[1] = PW_MAX_LEN;    /* max PW1 length */
    c4[2] = 0x00;          /* RFU / no resetting code */
    c4[3] = PW_MAX_LEN;    /* max PW3 length */
    c4[4] = s_pw1_retry;
    c4[5] = 0x00;          /* RC retries (not implemented) */
    c4[6] = s_pw3_retry;
}

/* Key Information DO 0xDE (OpenPGP 3.4 §4.4.3.9): per-slot {key-ref, status}.
 * Status: 0 = not present, 1 = generated on card, 2 = imported. */
static uint16_t fill_key_info(uint8_t de[6])
{
    for (int i = 0; i < OPENPGP_SLOT_COUNT; i++) {
        de[2*i]     = (uint8_t)(i + 1);
        de[2*i + 1] = s_keys[i].set ? s_keys[i].origin : 0x00;
    }
    return 6;
}

/* UIF DO for a slot (0x00D6 sig / 0x00D7 dec / 0x00D8 aut): byte0 != 0 → touch. */
static bool uif_required(uint16_t uif_tag)
{
    const uint8_t *v; uint16_t n;
    return openpgp_do_get(uif_tag, &v, &n) && n >= 1 && v[0] != 0x00;
}

/* Only the algorithms this card implements may be written to C1/C2/C3 —
 * accepting anything else would let gpg build keys the card cannot use. */
static bool algo_attrs_acceptable(uint16_t tag, const uint8_t *v, uint16_t n)
{
    static const uint8_t P256_ECDSA[9] = {0x13,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    if (tag == 0x00C1u || tag == 0x00C3u)
        return n == sizeof(P256_ECDSA) && memcmp(v, P256_ECDSA, n) == 0;
    if (tag == 0x00C2u)
        return n == sizeof(FACTORY_C2) && memcmp(v, FACTORY_C2, n) == 0;
    return true;
}

/* BER-TLV sibling scanner.
 * Scans TLV siblings in buf[0..buf_len-1] for the first occurrence of `tag`.
 * Returns a pointer to the first value byte and sets *vlen_out; returns NULL
 * if the tag is not found or the buffer is malformed/truncated.
 * Supports 1-byte and 2-byte tags (multi-byte indicator: lower 5 bits == 0x1F).
 * Supports short-form length (< 0x80) and 0x81 / 0x82 long-form lengths.
 * NEVER reads out of bounds — treats any truncation as malformed. */
static const uint8_t *tlv_find(const uint8_t *buf, uint16_t buf_len,
                               uint16_t tag, uint16_t *vlen_out)
{
    if (!buf || buf_len == 0) return NULL;
    uint16_t pos = 0;
    while (pos < buf_len) {
        /* --- tag --- */
        uint16_t cur_tag;
        if ((buf[pos] & 0x1Fu) == 0x1Fu) {
            /* multi-byte tag: need one more byte, MSB must be 0 */
            if (pos + 1u >= buf_len) return NULL;
            cur_tag = ((uint16_t)buf[pos] << 8) | buf[pos + 1u];
            pos += 2u;
        } else {
            cur_tag = buf[pos];
            pos += 1u;
        }
        if (pos >= buf_len) return NULL;

        /* --- length --- */
        uint32_t vlen;
        if (buf[pos] < 0x80u) {
            vlen  = buf[pos];
            pos  += 1u;
        } else if (buf[pos] == 0x81u) {
            if (pos + 1u >= buf_len) return NULL;
            vlen  = buf[pos + 1u];
            pos  += 2u;
        } else if (buf[pos] == 0x82u) {
            if (pos + 2u >= buf_len) return NULL;
            vlen  = ((uint32_t)buf[pos + 1u] << 8) | buf[pos + 2u];
            pos  += 3u;
        } else {
            return NULL; /* indefinite / reserved length form */
        }

        /* bounds check: value must fit in remaining buffer */
        if ((uint32_t)pos + vlen > (uint32_t)buf_len) return NULL;

        if (cur_tag == tag) {
            if (vlen_out) *vlen_out = (uint16_t)vlen;
            return buf + pos;
        }
        pos += (uint16_t)vlen;
    }
    return NULL; /* tag not found */
}

/* Sibling-level TLV iterator.
 * On entry *pos is the index of the next sibling's first byte.
 * Reads tag (1-2 bytes) and length (short/0x81/0x82) and updates:
 *   *tag  — decoded tag value
 *   *vlen — declared value length
 *   *pos  — updated to point at the first value byte (value start)
 * Returns false at end of buffer (normal termination) or on truncation.
 * For standard TLV: caller advances *pos by *vlen to reach the next sibling.
 * For 7F48 template pairs (no value bytes): omit the advance; *pos already
 * points at the next (tag, length) pair. */
static bool tlv_iter_next(const uint8_t *buf, uint16_t len, uint16_t *pos,
                           uint16_t *tag, uint16_t *vlen)
{
    if (*pos >= len) return false;
    uint16_t p = *pos;

    /* tag: 1 or 2 bytes */
    uint16_t cur_tag;
    if ((buf[p] & 0x1Fu) == 0x1Fu) {
        if (p + 1u >= len) return false;
        cur_tag = ((uint16_t)buf[p] << 8) | buf[p + 1u];
        p += 2u;
    } else {
        cur_tag = buf[p];
        p += 1u;
    }
    if (p >= len) return false;

    /* length: short-form / 0x81 one-byte / 0x82 two-byte */
    uint16_t el;
    if (buf[p] < 0x80u) {
        el = buf[p]; p += 1u;
    } else if (buf[p] == 0x81u) {
        if (p + 1u >= len) return false;
        el = buf[p + 1u]; p += 2u;
    } else if (buf[p] == 0x82u) {
        if (p + 2u >= len) return false;
        el = ((uint16_t)buf[p + 1u] << 8) | buf[p + 2u]; p += 3u;
    } else {
        return false; /* indefinite / reserved length form */
    }

    *tag  = cur_tag;
    *vlen = el;
    *pos  = p; /* value start */
    return true;
}

/* CRT tag → slot index, or -1. */
static int slot_from_crt(uint8_t crt)
{
    switch (crt) {
    case 0xB6: return OPENPGP_SLOT_SIG;
    case 0xB8: return OPENPGP_SLOT_DEC;
    case 0xA4: return OPENPGP_SLOT_AUT;
    default:   return -1;
    }
}

/* Reset the signature counter (DO 0x93) — required when a new SIG key is
 * generated or imported (OpenPGP 3.4 §7.2.14). */
static void ds_counter_reset(void)
{
    static const uint8_t zero[3] = {0, 0, 0};
    openpgp_do_put(0x0093u, zero, 3);
}

/* Slot's algorithm from the live algo-attrs DO (C1/C2/C3 leading byte). */
static uint8_t slot_algo(int slot)
{
    static const uint16_t attr_tag[OPENPGP_SLOT_COUNT] = {0x00C1u, 0x00C2u, 0x00C3u};
    const uint8_t *v; uint16_t n;
    if (openpgp_do_get(attr_tag[slot], &v, &n) && n >= 1) return v[0];
    return PGP_ALGO_ECDSA_P256;
}

/* Build the 7F49 public-key response for a populated slot.
 * P-256: 86 holds 65 B (04||X||Y).  X25519: 86 holds 0x40 || pub(32) —
 * the OpenPGP "native curve" point format gpg expects for cv25519.
 * Shared by READ PUBLIC KEY and (Task 5) GENERATE. */
static uint16_t build_pubkey_response(int slot, uint8_t *out, uint16_t out_max)
{
    const pgp_key_t *k = &s_keys[slot];
    if (!s_hooks->pubkey) return sw_only(out, out_max, SW_REF_NOT_FOUND);

    uint8_t raw[65]; uint16_t raw_n = 0;
    if (!s_hooks->pubkey(k->algo, k->d, raw, &raw_n))
        return sw_only(out, out_max, SW_REF_NOT_FOUND);

    uint8_t point[66]; uint16_t point_n;
    if (k->algo == PGP_ALGO_ECDH) {            /* X25519 → 0x40-prefixed */
        if (raw_n != 32) return sw_only(out, out_max, 0x6F00u);
        point[0] = 0x40; memcpy(point + 1, raw, 32); point_n = 33;
    } else {
        if (raw_n != 65) return sw_only(out, out_max, 0x6F00u);
        memcpy(point, raw, 65); point_n = 65;
    }

    uint8_t inner[70];
    uint16_t ioff = tlv_append(inner, sizeof(inner), 0, 0x86u, point, point_n);
    if (ioff == 0) return sw_only(out, out_max, 0x6F00u);
    uint8_t body[80];
    uint16_t off = tlv_append(body, sizeof(body), 0, 0x7F49u, inner, ioff);
    if (off == 0) return sw_only(out, out_max, 0x6F00u);
    return respond(out, out_max, body, off, SW_OK);
}

/* ------------------------------------------------------------------ */
/* Public API — lifecycle                                              */
/* ------------------------------------------------------------------ */

void openpgp_card_init(const openpgp_card_hooks_t *hooks)
{
    s_hooks = hooks;

    /* Reset session state */
    s_selected          = false;
    s_terminated        = false;
    s_pw1_sign_verified = false;
    s_pw1_user_verified = false;
    s_pw3_verified      = false;

    /* Factory PIN baseline. On target, NVS-backed PIN state (Task 6,
     * openpgp_card_load) overrides these after init; until then a fresh
     * boot must yield a usable card, not a blocked one (retry == 0). */
    pins_factory();
}

bool openpgp_card_ensure_defaults(void)
{
    const uint8_t *v;
    uint16_t n;
    unsigned failures = 0;

    /* Populate any absent factory DOs without touching existing ones.
     * Returns false if any openpgp_do_put fails (table full / oversized). */
#define ENSURE(tag_, data_, len_) \
    do { if (!openpgp_do_get((tag_), &v, &n)) \
             if (!openpgp_do_put((tag_), (data_), (len_))) failures++; } while (0)

    ENSURE(0x004F, FACTORY_AID,  16);
    ENSURE(0x5F52, FACTORY_HIST, 10);
    ENSURE(0x00C0, FACTORY_C0,   10);
    ENSURE(0x00C1, FACTORY_C1,    9);
    ENSURE(0x00C2, FACTORY_C2,   11);
    ENSURE(0x00C3, FACTORY_C3,    9);
    ENSURE(0x00C5, NULL,         60);  /* fingerprints: zero-filled    */
    ENSURE(0x00C6, NULL,         60);  /* CA fingerprints: zero-filled */
    ENSURE(0x00CD, NULL,         12);  /* generation timestamps: zero  */
    ENSURE(0x00D6, FACTORY_D6,    2);
    ENSURE(0x00D7, FACTORY_D7,    2);
    ENSURE(0x00D8, FACTORY_D8,    2);
    ENSURE(0x005B, NULL,          0);  /* name: empty                  */
    ENSURE(0x5F2D, (const uint8_t *)"en", 2);
    ENSURE(0x5F35, (const uint8_t *)"9",  1);
    ENSURE(0x5F50, NULL,          0);  /* URL: empty                   */
    ENSURE(0x005E, NULL,          0);  /* login data: empty — gnupg do_learn_status
                                        * aborts the full LEARN chain on 6A88 here,
                                        * silently skipping CHV-STATUS/KEY-FPR/etc. */
    ENSURE(0x7F74, FACTORY_7F74,  3);  /* General Feature Management: scdaemon
                                        * sets extcap.has_button from this DO    */
    ENSURE(0x0093, NULL,          3);  /* DS counter: zero             */

#undef ENSURE

    /* Phase-1 cards shipped C2 = ECDH P-256; Phase 2 decrypts with X25519
     * only.  Upgrade the stale factory value in place — safe: PSO:DECIPHER
     * did not exist in Phase 1, so no key material depends on the old attrs. */
    static const uint8_t LEGACY_C2_P256[9] = {0x12,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07};
    if (openpgp_do_get(0x00C2u, &v, &n) && n == 9 && memcmp(v, LEGACY_C2_P256, 9) == 0)
        if (!openpgp_do_put(0x00C2u, FACTORY_C2, sizeof(FACTORY_C2))) failures++;

    return (failures == 0);
}

bool openpgp_card_factory_reset(void)
{
    bool ok = true;

    /* 1. Reset session state */
    s_selected          = false;
    s_pw1_sign_verified = false;
    s_pw1_user_verified = false;
    s_pw3_verified      = false;

    /* 2. Clear imported keys (all slots) and persist the cleared state */
    memset(s_keys, 0, sizeof(s_keys));
    if (!key_persist()) ok = false;

    /* 3. Wipe DO store */
    openpgp_do_reset();

    /* 4. Restore factory PINs and retry counters, then persist them */
    pins_factory();
    if (!pin_persist()) ok = false;

    /* 5. Populate all factory-default DOs */
    if (!openpgp_card_ensure_defaults()) ok = false;

    return ok;
}

bool openpgp_card_key_is_set(void)
{
    return s_keys[OPENPGP_SLOT_SIG].set != 0;
}

void openpgp_card_set_serial(const uint8_t serial[4])
{
    uint8_t aid[16];
    const uint8_t *v;
    uint16_t n;

    if (openpgp_do_get(0x004F, &v, &n) && n == 16)
        memcpy(aid, v, 16);
    else
        memcpy(aid, FACTORY_AID, 16);

    memcpy(aid + 10, serial, 4);
    openpgp_do_put(0x004F, aid, 16);
}

void openpgp_card_set_crypto_health(bool ok)
{
    s_crypto_ok = ok;
}

/* Constant-time PIN comparison.  Returns true iff a[0..alen-1] == b[0..blen-1]
 * with no early exit on mismatch, so the timing is independent of where the
 * first differing byte is.  A length difference makes the final `alen == blen`
 * false, but the byte loop still runs for min(alen,blen) iterations — do NOT
 * "optimise" it into an early return; lengths are public in the APDU framing,
 * no CT property is required there, and the loop bound keeps both reads in
 * bounds. */
static bool ct_pin_equal(const uint8_t *a, uint16_t alen,
                         const uint8_t *b, uint16_t blen)
{
    uint8_t  diff = (uint8_t)(alen ^ blen);
    uint16_t n    = alen < blen ? alen : blen;
    for (uint16_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0 && alen == blen;
}

/* ------------------------------------------------------------------ */
/* APDU dispatch                                                       */
/* ------------------------------------------------------------------ */

uint16_t openpgp_card_apdu(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max)
{
    apdu_t a;
    if (!apdu_parse(in, in_len, &a))
        return sw_only(out, out_max, 0x6700); /* wrong length */

    /* Terminated DF (after INS 0xE6): only ACTIVATE FILE revives the card. */
    if (s_terminated) {
        if (a.ins == 0x44) {            /* ACTIVATE FILE */
            s_terminated = false;
            openpgp_card_factory_reset();
            return sw_only(out, out_max, SW_OK);
        }
        return sw_only(out, out_max, 0x6285u);  /* selected file in terminated state */
    }

    /* ---- SELECT (always available, even before selection) ---- */
    if (a.ins == 0xA4) {
        if (a.p1 != 0x04)
            return sw_only(out, out_max, SW_REF_NOT_FOUND);
        if (a.lc < 6 || !a.data ||
            memcmp(a.data, OPGP_AID_PREFIX, 6) != 0)
            return sw_only(out, out_max, SW_REF_NOT_FOUND);
        s_selected          = true;
        s_pw1_sign_verified = false;
        s_pw1_user_verified = false;
        s_pw3_verified      = false;
        return sw_only(out, out_max, SW_OK);
    }

    /* ---- TERMINATE DF (INS=0xE6) — gpg factory-reset, step 1 ----
     * Placed before the s_selected guard so it works whenever gpg sends it
     * (gpg always SELECTs first anyway). */
    if (a.ins == 0xE6) {
        /* Allowed if PW3 verified, OR both PINs are blocked (the escape hatch
         * gpg uses to reset a locked card). */
        if (!(s_pw3_verified || (s_pw1_retry == 0 && s_pw3_retry == 0)))
            return sw_only(out, out_max, SW_SEC_NOT_SAT);
        openpgp_card_factory_reset();
        /* s_terminated is RAM-only by design: the destructive wipe is already
         * persisted by factory_reset(), so a power-cycle just brings up an
         * already-reset, usable card.  The flag only gates the in-session
         * ACTIVATE handshake (gpg sends ACTIVATE within milliseconds). */
        s_terminated = true;
        return sw_only(out, out_max, SW_OK);
    }
    /* ---- ACTIVATE FILE (INS=0x44) — no-op when not terminated ---- */
    if (a.ins == 0x44)
        return sw_only(out, out_max, SW_OK);

    /* All other commands require the applet to be selected */
    if (!s_selected)
        return sw_only(out, out_max, SW_COND_NOT_SAT);

    /* ---- VERIFY (INS=0x20) ---- */
    if (a.ins == 0x20) {
        bool is_pw1 = (a.p2 == 0x81 || a.p2 == 0x82);
        bool is_pw3 = (a.p2 == 0x83);
        if (!is_pw1 && !is_pw3)
            return sw_only(out, out_max, SW_WRONG_P1P2);

        uint8_t *retry  = is_pw1 ? &s_pw1_retry  : &s_pw3_retry;
        uint8_t *pin    = is_pw1 ? s_pw1 : s_pw3;
        uint8_t  pinlen = is_pw1 ? s_pw1_len : s_pw3_len;

        if (*retry == 0)
            return sw_only(out, out_max, SW_AUTH_BLOCKED);

        /* Check-only (no data): return current retry count */
        if (a.lc == 0)
            return sw_only(out, out_max, (uint16_t)(0x63C0u | *retry));

        /* PIN comparison — constant-time to avoid length/position oracle */
        if (!ct_pin_equal(a.data, a.lc, pin, pinlen)) {
            /* duplicated in VERIFY/CHANGE; extract pin_wrong() if a third user appears (e.g. INS 2C) */
            (*retry)--;
            pin_persist();
            return sw_only(out, out_max, (uint16_t)(0x63C0u | *retry));
        }

        /* Correct PIN — reset counter, set mode-specific verified flag.
         * Only persist when the counter actually changed (was below max), to
         * avoid an NVS write on every routine VERIFY of an unblocked card. */
        uint8_t old_retry = *retry;
        *retry = (is_pw1 ? PW1_RETRY_MAX : PW3_RETRY_MAX);
        if      (a.p2 == 0x81) s_pw1_sign_verified = true;
        else if (a.p2 == 0x82) s_pw1_user_verified = true;
        else                   s_pw3_verified       = true;
        if (old_retry != *retry) pin_persist();
        return sw_only(out, out_max, SW_OK);
    }

    /* ---- CHANGE REFERENCE DATA (INS=0x24) ---- */
    if (a.ins == 0x24) {
        /* P1 must be 0x00; P2 must be 0x81 (PW1) or 0x83 (PW3) */
        if (a.p1 != 0x00)
            return sw_only(out, out_max, SW_WRONG_P1P2);
        bool is_pw1c = (a.p2 == 0x81);
        bool is_pw3c = (a.p2 == 0x83);
        if (!is_pw1c && !is_pw3c)
            return sw_only(out, out_max, SW_WRONG_P1P2);

        uint8_t *retry     = is_pw1c ? &s_pw1_retry  : &s_pw3_retry;
        uint8_t *pin       = is_pw1c ? s_pw1          : s_pw3;
        uint8_t *pin_len   = is_pw1c ? &s_pw1_len     : &s_pw3_len;
        uint8_t  retry_max = is_pw1c ? PW1_RETRY_MAX  : PW3_RETRY_MAX;
        uint8_t  min_len   = is_pw1c ? (uint8_t)PW1_DEFAULT_LEN
                                     : (uint8_t)PW3_DEFAULT_LEN;

        if (*retry == 0)
            return sw_only(out, out_max, SW_AUTH_BLOCKED);

        /* Frame validation: data = old_pin ‖ new_pin split at current length.
         * Malformed frames return 6A80 with NO retry decrement. */
        uint8_t old_len = *pin_len;
        if (a.lc <= old_len)
            return sw_only(out, out_max, SW_WRONG_DATA);
        uint8_t new_len = (uint8_t)(a.lc - old_len);
        if (new_len < min_len || new_len > PW_MAX_LEN)
            return sw_only(out, out_max, SW_WRONG_DATA);

        /* Verify old PIN — constant-time compare; mismatch decrements counter */
        if (!ct_pin_equal(a.data, old_len, pin, old_len)) {
            (*retry)--;
            pin_persist();
            return sw_only(out, out_max, (uint16_t)(0x63C0u | *retry));
        }

        /* Correct old PIN — store new PIN, reset retry, clear verified flags */
        memcpy(pin, a.data + old_len, new_len);
        *pin_len = new_len;
        *retry   = retry_max;
        if (is_pw1c) {
            s_pw1_sign_verified = false;
            s_pw1_user_verified = false;
        } else {
            s_pw3_verified = false;
        }
        pin_persist();
        return sw_only(out, out_max, SW_OK);
    }

    /* ---- GET DATA (INS=0xCA) ---- */
    if (a.ins == 0xCA) {
        uint16_t       tag = ((uint16_t)a.p1 << 8) | a.p2;
        const uint8_t *v;
        uint16_t        n;

/* Abort the response build on TLV overflow: corrupt-but-9000 is the worst
 * failure mode.  References out/out_max from openpgp_card_apdu's scope. */
#define APPEND_OR_FAIL(buf_, max_, off_, tag_, v_, n_) \
    do { (off_) = tlv_append((buf_), (max_), (off_), (tag_), (v_), (n_)); \
         if ((off_) == 0) return sw_only(out, out_max, 0x6F00); } while (0)

        /* ---- 6E: Application Related Data (constructed) ---- */
        /* CEILING: this response is ~254 B (data) + 2 SW = 256 = the short-APDU
         * Le=256 maximum.  There is NO GET RESPONSE chaining / extended-length
         * here, so 256 is a hard limit.  Adding any DO (or a longer value) to
         * the 73 template below will overflow a short-Le host read — if more is
         * ever needed, add 61xx/00C0 response chaining FIRST. */
        if (tag == 0x006Eu) {
            uint8_t  body[256];
            uint16_t off = 0;

            /* 4F: AID */
            v = NULL; n = 0; openpgp_do_get(0x004F, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x004Fu, v, n);

            /* 5F52: Historical bytes (two-byte tag) */
            v = NULL; n = 0; openpgp_do_get(0x5F52u, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x5F52u, v, n);

            /* 7F74: General Feature Management (two-byte tag).
             * OpenPGP 3.4 §4.4.3 places it here, between 5F52 and 73.
             * scdaemon reads it directly to set extcap.has_button. */
            v = NULL; n = 0; openpgp_do_get(0x7F74u, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x7F74u, v, n);

            /* 73: Discretionary DOs — build inner content first */
            {
                uint8_t  inner[256];
                uint16_t ioff = 0;

                v = NULL; n = 0; openpgp_do_get(0x00C0u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC0u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00C1u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC1u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00C2u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC2u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00C3u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC3u, v, n);

                /* C4: synthesised from live retry counters — never stored */
                uint8_t c4[7];
                fill_pw_status(c4);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC4u, c4, 7);

                v = NULL; n = 0; openpgp_do_get(0x00C5u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC5u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00C6u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xC6u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00CDu, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xCDu, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00D6u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xD6u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00D7u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xD7u, v, n);

                v = NULL; n = 0; openpgp_do_get(0x00D8u, &v, &n);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xD8u, v, n);

                /* DE: Key Information — synthesised from slot origins */
                uint8_t de[6];
                fill_key_info(de);
                APPEND_OR_FAIL(inner, sizeof(inner), ioff, 0xDEu, de, 6);

                APPEND_OR_FAIL(body, sizeof(body), off, 0x73u, inner, ioff);
            }

            return respond(out, out_max, body, off, SW_OK);
        }

        /* ---- 65: Cardholder Related Data (constructed) ---- */
        if (tag == 0x0065u) {
            uint8_t  body[64];
            uint16_t off = 0;

            v = NULL; n = 0; openpgp_do_get(0x005Bu, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x5Bu, v, n);

            v = NULL; n = 0; openpgp_do_get(0x5F2Du, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x5F2Du, v, n);

            v = NULL; n = 0; openpgp_do_get(0x5F35u, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x5F35u, v, n);

            return respond(out, out_max, body, off, SW_OK);
        }

        /* ---- 7A: Security Support Template (constructed) ---- */
        if (tag == 0x007Au) {
            uint8_t  body[16];
            uint16_t off = 0;

            v = NULL; n = 0; openpgp_do_get(0x0093u, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x93u, v, n);

            return respond(out, out_max, body, off, SW_OK);
        }

#undef APPEND_OR_FAIL

        /* ---- C4: PW status bytes (synthesised — never stored) ---- */
        if (tag == 0x00C4u) {
            uint8_t c4[7];
            fill_pw_status(c4);
            return respond(out, out_max, c4, 7, SW_OK);
        }

        /* ---- DE: Key Information (synthesised from slot origins) ---- */
        if (tag == 0x00DEu) {
            uint8_t de[6];
            fill_key_info(de);
            return respond(out, out_max, de, 6, SW_OK);
        }

        /* ---- All others: flat store lookup ---- */
        if (!openpgp_do_get(tag, &v, &n))
            return sw_only(out, out_max, SW_REF_NOT_FOUND);
        return respond(out, out_max, v, n, SW_OK);
    }

    /* ---- PUT DATA (INS=0xDA) ---- */
    if (a.ins == 0xDA) {
        if (!s_pw3_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);

        uint16_t tag = ((uint16_t)a.p1 << 8) | a.p2;

        /* C4: PW1 status / forcesig.  Byte0 0=valid for one PSO:CDS (forced),
         * 1=valid for several.  Stored as a 1-byte DO; GET DATA C4 + fill_pw_status
         * reflect byte0.  Older gpg may send 4 bytes — use byte0. */
        if (tag == 0x00C4u) {
            if (a.lc < 1 || (a.data[0] != 0x00 && a.data[0] != 0x01))
                return sw_only(out, out_max, SW_WRONG_DATA);
            uint8_t v0 = a.data[0];
            if (!openpgp_do_put(0x00C4u, &v0, 1))
                return sw_only(out, out_max, SW_WRONG_DATA);
            return sw_only(out, out_max, SW_OK);
        }

        /* Fingerprint slices: C7/C8/C9 → write FP_SLICE_LEN-byte slice into C5 */
        if (tag == 0x00C7u || tag == 0x00C8u || tag == 0x00C9u) {
            if (a.lc != FP_SLICE_LEN)
                return sw_only(out, out_max, SW_WRONG_DATA);
            uint8_t        c5[3 * FP_SLICE_LEN];
            const uint8_t *cur; uint16_t cur_n;
            if (openpgp_do_get(0x00C5u, &cur, &cur_n) && cur_n == 3 * FP_SLICE_LEN)
                memcpy(c5, cur, 3 * FP_SLICE_LEN);
            else
                memset(c5, 0, 3 * FP_SLICE_LEN);
            uint16_t slice_off = (tag == 0x00C7u) ? 0u :
                                 (tag == 0x00C8u) ? FP_SLICE_LEN : (2u * FP_SLICE_LEN);
            memcpy(c5 + slice_off, a.data, FP_SLICE_LEN);
            if (!openpgp_do_put(0x00C5u, c5, 3 * FP_SLICE_LEN))
                return sw_only(out, out_max, SW_WRONG_DATA);
            return sw_only(out, out_max, SW_OK);
        }

        /* Generation timestamp slices: CE/CF/D0 → write TS_SLICE_LEN-byte slice into CD */
        if (tag == 0x00CEu || tag == 0x00CFu || tag == 0x00D0u) {
            if (a.lc != TS_SLICE_LEN)
                return sw_only(out, out_max, SW_WRONG_DATA);
            uint8_t        cd[3 * TS_SLICE_LEN];
            const uint8_t *cur; uint16_t cur_n;
            if (openpgp_do_get(0x00CDu, &cur, &cur_n) && cur_n == 3 * TS_SLICE_LEN)
                memcpy(cd, cur, 3 * TS_SLICE_LEN);
            else
                memset(cd, 0, 3 * TS_SLICE_LEN);
            uint16_t slice_off = (tag == 0x00CEu) ? 0u :
                                 (tag == 0x00CFu) ? TS_SLICE_LEN : (2u * TS_SLICE_LEN);
            memcpy(cd + slice_off, a.data, TS_SLICE_LEN);
            if (!openpgp_do_put(0x00CDu, cd, 3 * TS_SLICE_LEN))
                return sw_only(out, out_max, SW_WRONG_DATA);
            return sw_only(out, out_max, SW_OK);
        }

        /* Algorithm attributes C1/C2/C3: reject anything the card can't use. */
        if ((tag == 0x00C1u || tag == 0x00C2u || tag == 0x00C3u) &&
            !algo_attrs_acceptable(tag, a.data, a.lc))
            return sw_only(out, out_max, SW_WRONG_DATA);

        /* Generic PUT DATA */
        if (!openpgp_do_put(tag, a.data, a.lc))
            return sw_only(out, out_max, SW_WRONG_DATA);
        return sw_only(out, out_max, SW_OK);
    }

    /* ---- PUT DATA, odd (INS=0xDB) — Extended Header List key import ---- */
    if (a.ins == 0xDB) {
        /* Only P1P2 = 3FFF (key import) is supported in Phase 1 */
        if (a.p1 != 0x3Fu || a.p2 != 0xFFu)
            return sw_only(out, out_max, SW_WRONG_P1P2);
        if (!s_pw3_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);

        /* Outer container: 4D (Extended Header List) */
        uint16_t       inner_len = 0;
        const uint8_t *inner     = tlv_find(a.data, a.lc, 0x4Du, &inner_len);
        if (!inner)
            return sw_only(out, out_max, SW_WRONG_DATA);

        /* Locate the key-slot CRT.  gpg sends exactly one of B6 (SIG) / B8
         * (DEC) / A4 (AUT); scan in that order, first match wins.  No
         * recognised CRT → 6A80. */
        int      slot = -1;
        uint16_t crt_vlen = 0;
        static const uint16_t crt_tags[OPENPGP_SLOT_COUNT] =
            {0x00B6u, 0x00B8u, 0x00A4u};
        for (unsigned ci = 0; ci < OPENPGP_SLOT_COUNT; ci++) {
            if (tlv_find(inner, inner_len, crt_tags[ci], &crt_vlen)) {
                slot = slot_from_crt((uint8_t)crt_tags[ci]);
                break;
            }
        }
        if (slot < 0)
            return sw_only(out, out_max, SW_WRONG_DATA);

        uint8_t algo = slot_algo(slot);

        /* 7F48: Cardholder private key template — (tag, length) pairs only,
         * no values; each pair describes one element's byte-size in 5F48. */
        uint16_t       tmpl_len = 0;
        const uint8_t *tmpl     = tlv_find(inner, inner_len, 0x7F48u, &tmpl_len);
        if (!tmpl) return sw_only(out, out_max, SW_WRONG_DATA);

        /* Walk template with tlv_iter_next to find tag 92 (private scalar)
         * and accumulate its byte-offset in 5F48.  The 7F48 template contains
         * only (tag, length) pairs with no value bytes, so we do NOT advance
         * *pos by vlen between iterations. */
        uint32_t d_offset = 0;  /* semantic uint32_t: offset into 5F48 key material */
        bool     found_92 = false;
        uint16_t scalar_len = 0;  /* declared byte size of element 92 */
        uint16_t ti = 0, etag, elen;
        while (tlv_iter_next(tmpl, tmpl_len, &ti, &etag, &elen)) {
            if (etag == 0x92u) {
                /* ECDSA P-256: scalar is exactly 32 B.  X25519 (ECDH): the
                 * cv25519 secret may arrive 1..32 B and is left-padded below. */
                if (algo == PGP_ALGO_ECDH) {
                    if (elen < 1u || elen > PGP_P256_SCALAR_BYTES)
                        return sw_only(out, out_max, SW_WRONG_DATA);
                } else {
                    if (elen != PGP_P256_SCALAR_BYTES)
                        return sw_only(out, out_max, SW_WRONG_DATA);
                }
                scalar_len = elen;
                found_92   = true;
                break;
            }
            d_offset += elen; /* accumulate 5F48 offset for elements before 92 */
            /* 7F48 template has no value bytes; next (tag,len) pair follows. */
        }
        if (!found_92) return sw_only(out, out_max, SW_WRONG_DATA);

        /* 5F48: concatenated key material; private scalar at
         * [d_offset .. d_offset + scalar_len - 1] */
        uint16_t       km_len = 0;
        const uint8_t *km     = tlv_find(inner, inner_len, 0x5F48u, &km_len);
        if (!km) return sw_only(out, out_max, SW_WRONG_DATA);
        if (d_offset + scalar_len > (uint32_t)km_len)
            return sw_only(out, out_max, SW_WRONG_DATA);

        /* Build the 32-byte big-endian scalar.  P-256 always fills all 32 B;
         * a short X25519 secret is left-padded with leading zeros. */
        uint8_t d[PGP_P256_SCALAR_BYTES];
        memset(d, 0, sizeof(d));
        memcpy(d + (PGP_P256_SCALAR_BYTES - scalar_len), km + d_offset, scalar_len);

        /* Reject an all-zero private scalar — provably invalid and exploitable.
         * (d >= N is rejected at sign time by mbedtls; the zero case is caught
         * here before we persist the material.)  Checked on the padded buffer. */
        {
            bool all_zero = true;
            for (uint16_t zi = 0; zi < PGP_P256_SCALAR_BYTES; zi++) {
                if (d[zi] != 0u) { all_zero = false; break; }
            }
            if (all_zero) return sw_only(out, out_max, SW_WRONG_DATA);
        }

        /* Durable key import: write to RAM then persist to NVS.
         * On NVS failure scrub the slot so host-visible state stays coherent;
         * return 6F00 so the host can detect the failure and retry. */
        s_keys[slot].set    = 1u;
        s_keys[slot].algo   = algo;
        s_keys[slot].origin = 2u;   /* imported */
        memcpy(s_keys[slot].d, d, PGP_P256_SCALAR_BYTES);
        memset(d, 0, sizeof(d));    /* scrub the transient scalar copy */
        if (!key_persist()) {
            memset(&s_keys[slot], 0, sizeof(s_keys[slot]));
            return sw_only(out, out_max, 0x6F00u);
        }
        if (slot == OPENPGP_SLOT_SIG) ds_counter_reset();
        return sw_only(out, out_max, SW_OK);
    }

    /* ---- PSO (INS=0x2A): COMPUTE DIGITAL SIGNATURE / DECIPHER ---- */
    if (a.ins == 0x2A) {
        /* ---- PSO: DECIPHER (P1=0x80, P2=0x86) — X25519 ECDH ---- */
        if (a.p1 == 0x80 && a.p2 == 0x86) {
            if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);

            const pgp_key_t *k = &s_keys[OPENPGP_SLOT_DEC];
            if (!k->set) return sw_only(out, out_max, SW_REF_NOT_FOUND);
            if (k->algo != PGP_ALGO_ECDH)
                return sw_only(out, out_max, SW_COND_NOT_SAT);
            if (!s_pw1_user_verified)
                return sw_only(out, out_max, SW_SEC_NOT_SAT);

            /* data = A6{ 7F49{ 86 <ephemeral public point> } } */
            uint16_t a6_n; const uint8_t *a6 = tlv_find(a.data, a.lc, 0x00A6u, &a6_n);
            if (!a6) return sw_only(out, out_max, SW_WRONG_DATA);
            uint16_t t_n; const uint8_t *t = tlv_find(a6, a6_n, 0x7F49u, &t_n);
            if (!t) return sw_only(out, out_max, SW_WRONG_DATA);
            uint16_t p_n; const uint8_t *p = tlv_find(t, t_n, 0x0086u, &p_n);
            if (!p) return sw_only(out, out_max, SW_WRONG_DATA);
            /* X25519 u-coordinate: 32 B; tolerate the 0x40 native-format prefix. */
            if (p_n == 33 && p[0] == 0x40) { p++; p_n = 32; }
            if (p_n != 32) return sw_only(out, out_max, SW_WRONG_DATA);

            /* UIF D7 gate (after input validation: don't burn a touch on garbage). */
            if (uif_required(0x00D7u)) {
                int cs = s_hooks->confirm();
                if (cs != 1) return sw_only(out, out_max, SW_COND_NOT_SAT);
            }
            /* NOTE: mode-82 verification is NOT consumed — the C4 validity
             * byte applies to PSO:CDS only (OpenPGP 3.4 §7.2.2). */

            uint8_t shared[32]; uint16_t shared_n = 0;
            if (!s_hooks->ecdh ||
                !s_hooks->ecdh(k->d, p, p_n, shared, &shared_n))
                return sw_only(out, out_max, SW_COND_NOT_SAT);
            /* Defence-in-depth: never copy past the buffer if a future hook
             * mis-reports its length (the contract fixes X25519 output at 32 B). */
            if (shared_n > sizeof(shared)) shared_n = sizeof(shared);
            return respond(out, out_max, shared, shared_n, SW_OK);
        }

        /* ---- PSO: Compute Digital Signature (P1=0x9E, P2=0x9A) ---- */
        if (a.p1 != 0x9E || a.p2 != 0x9A)
            return sw_only(out, out_max, SW_WRONG_P1P2);

        /* Crypto health gate — before any key/PIN check.
         * A failed KAT at boot means all crypto output is untrustworthy. */
        if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);

        /* Referenced data check: SIG-slot key must be imported first */
        if (!s_keys[OPENPGP_SLOT_SIG].set)
            return sw_only(out, out_max, SW_REF_NOT_FOUND);

        if (!s_pw1_sign_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);

        /* UIF gate: if DO 0xD6 byte[0] != 0, call confirm hook */
        if (uif_required(0x00D6u)) {
            int cs = s_hooks->confirm();
            if (cs != 1)
                return sw_only(out, out_max, SW_COND_NOT_SAT);
        }

        /* Consume PW1 sign authorisation before attempting the operation —
         * exactly one attempt per VERIFY 81, regardless of sign() success or failure.
         * Consumed here (not at the UIF gate) so a UIF denial preserves the token
         * but a crypto failure always burns it.
         * Only consume in "valid for one signature" mode (C4 byte0 == 0);
         * byte0 == 1 keeps the token (gpg "forcesig off"). */
        {
            const uint8_t *c4v; uint16_t c4n;
            bool multi = openpgp_do_get(0x00C4u, &c4v, &c4n) && c4n >= 1 && c4v[0] == 0x01;
            if (!multi) s_pw1_sign_verified = false;
        }

        /* Invoke sign hook with private scalar */
        uint8_t  sig[256];
        uint16_t sig_len = 0;
        if (!s_hooks->sign(s_keys[OPENPGP_SLOT_SIG].d, a.data, a.lc, sig, &sig_len))
            return sw_only(out, out_max, SW_COND_NOT_SAT);

        /* Increment DS counter DO 0x93 (3-byte big-endian, persisted by DO store) */
        {
            const uint8_t *cv; uint16_t cn;
            uint8_t ctr[3] = {0u, 0u, 0u};
            if (openpgp_do_get(0x0093u, &cv, &cn) && cn == 3u)
                memcpy(ctr, cv, 3u);
            uint32_t cnt = ((uint32_t)ctr[0] << 16) |
                           ((uint32_t)ctr[1] <<  8) | ctr[2];
            cnt++;
            ctr[0] = (uint8_t)(cnt >> 16);
            ctr[1] = (uint8_t)((cnt >> 8) & 0xFFu);
            ctr[2] = (uint8_t)(cnt & 0xFFu);
            openpgp_do_put(0x0093u, ctr, 3u);
        }

        return respond(out, out_max, sig, sig_len, SW_OK);
    }

    /* ---- INTERNAL AUTHENTICATE (INS=0x88) — SSH via gpg-agent ---- */
    if (a.ins == 0x88) {
        if (a.p1 != 0x00 || a.p2 != 0x00)
            return sw_only(out, out_max, SW_WRONG_P1P2);
        if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);

        const pgp_key_t *k = &s_keys[OPENPGP_SLOT_AUT];
        if (!k->set) return sw_only(out, out_max, SW_REF_NOT_FOUND);
        if (k->algo != PGP_ALGO_ECDSA_P256)
            return sw_only(out, out_max, SW_COND_NOT_SAT);
        if (!s_pw1_user_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);
        if (a.lc == 0 || a.lc > 64 || !a.data)
            return sw_only(out, out_max, SW_WRONG_DATA);

        if (uif_required(0x00D8u)) {
            int cs = s_hooks->confirm();
            if (cs != 1) return sw_only(out, out_max, SW_COND_NOT_SAT);
        }
        /* mode-82 is NOT consumed and the DS counter does NOT move —
         * both are PSO:CDS-only semantics (OpenPGP 3.4 §7.2.10/§7.2.13). */

        uint8_t sig[256]; uint16_t sig_len = 0;
        if (!s_hooks->sign(k->d, a.data, a.lc, sig, &sig_len))
            return sw_only(out, out_max, SW_COND_NOT_SAT);
        /* Defence-in-depth: never copy past the buffer if a future hook
         * mis-reports its length (P-256 r||s output is fixed at 64 B). */
        if (sig_len > sizeof(sig)) sig_len = sizeof(sig);
        return respond(out, out_max, sig, sig_len, SW_OK);
    }

    /* ---- GENERATE ASYMMETRIC KEY PAIR (INS=0x47) ---- */
    if (a.ins == 0x47) {
        if (a.p1 == 0x81) {  /* READ existing public key — do NOT generate */
            /* Data is a CRT tag (B6/B8/A4); map to a slot.  An unknown leading
             * byte means "referenced data not found" → 6A88. */
            if (a.lc == 0 || !a.data)
                return sw_only(out, out_max, SW_REF_NOT_FOUND);
            int slot = slot_from_crt(a.data[0]);
            if (slot < 0)
                return sw_only(out, out_max, SW_REF_NOT_FOUND);

            /* Crypto health gate — before any key check. */
            if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);

            if (!s_keys[slot].set)
                return sw_only(out, out_max, SW_REF_NOT_FOUND);

            if (!s_hooks->pubkey)
                return sw_only(out, out_max, SW_REF_NOT_FOUND);

            return build_pubkey_response(slot, out, out_max);
        }
        if (a.p1 == 0x80) {   /* on-device GENERATE */
            if (a.lc == 0 || !a.data)
                return sw_only(out, out_max, SW_WRONG_DATA);
            int slot = slot_from_crt(a.data[0]);
            if (slot < 0) return sw_only(out, out_max, SW_REF_NOT_FOUND);
            if (!s_crypto_ok) return sw_only(out, out_max, 0x6581u);
            if (!s_pw3_verified)
                return sw_only(out, out_max, SW_SEC_NOT_SAT);
            if (!s_hooks->genkey)
                return sw_only(out, out_max, SW_COND_NOT_SAT);

            uint8_t algo = slot_algo(slot);
            uint8_t d[32];
            if (!s_hooks->genkey(algo, d)) {
                memset(d, 0, sizeof(d));   /* scrub any partial scalar */
                return sw_only(out, out_max, SW_COND_NOT_SAT);
            }

            s_keys[slot].set    = 1;
            s_keys[slot].algo   = algo;
            s_keys[slot].origin = 1;            /* generated on-device */
            memcpy(s_keys[slot].d, d, 32);
            memset(d, 0, sizeof(d));            /* scrub the transient scalar */
            if (!key_persist()) {
                memset(&s_keys[slot], 0, sizeof(s_keys[slot]));
                return sw_only(out, out_max, 0x6F00u);
            }
            if (slot == OPENPGP_SLOT_SIG) ds_counter_reset();
            return build_pubkey_response(slot, out, out_max);
        }
        /* other P1 values fall through to 6D00 */
    }

    return sw_only(out, out_max, SW_INS_NOT_SUP);
}

/* ------------------------------------------------------------------ */
/* NVS persistence helpers — target only; host stubs + test counters  */
/* ------------------------------------------------------------------ */
#ifndef TEST_HOST
#include "esp_log.h"
#include "nvs_utils.h"
#include "keyboard_config.h"
static const char *TAG = "openpgp_card";

/* NVS blob v2 — new key "pgp_keys" (the v1 "pgp_key" single-slot blob is
 * migrated on first load and left in place, harmless). */
static bool key_persist(void)
{
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "pgp_keys",
                                             s_keys, sizeof(s_keys),
                                             "pgp_keys_ver", 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool pin_persist(void)
{
    pgp_pins_blob_t blob;
    memcpy(blob.pw1, s_pw1, PW_MAX_LEN);
    blob.pw1_len   = s_pw1_len;
    blob.pw1_retry = s_pw1_retry;
    memcpy(blob.pw3, s_pw3, PW_MAX_LEN);
    blob.pw3_len   = s_pw3_len;
    blob.pw3_retry = s_pw3_retry;
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "pgp_pins",
                                             &blob, sizeof(blob),
                                             "pgp_pins_ver", 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pin persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void openpgp_card_load(void)
{
    /* Load PIN/retry state — on fresh device the blob is absent; keep factory RAM state */
    uint32_t ver = 0;
    pgp_pins_blob_t blob;
    if (nvs_load_blob_with_total(STORAGE_NAMESPACE, "pgp_pins",
                                 &blob, sizeof(blob), "pgp_pins_ver", &ver) == ESP_OK) {
        /* Clamp loaded values; implausible lengths -> keep factory baseline. */
        if (blob.pw1_len < PW1_DEFAULT_LEN || blob.pw1_len > PW_MAX_LEN ||
            blob.pw3_len < PW3_DEFAULT_LEN || blob.pw3_len > PW_MAX_LEN) {
            ESP_LOGW(TAG, "pgp_pins blob invalid lengths — keeping factory state");
        } else {
            memcpy(s_pw1, blob.pw1, PW_MAX_LEN);
            s_pw1_len   = blob.pw1_len;
            s_pw1_retry = (blob.pw1_retry <= PW1_RETRY_MAX) ? blob.pw1_retry : PW1_RETRY_MAX;
            memcpy(s_pw3, blob.pw3, PW_MAX_LEN);
            s_pw3_len   = blob.pw3_len;
            s_pw3_retry = (blob.pw3_retry <= PW3_RETRY_MAX) ? blob.pw3_retry : PW3_RETRY_MAX;
        }
    }
    /* Load imported keys.  Try the v2 multi-slot blob first; on a
     * fresh-from-Phase-1 device fall back to the legacy v1 single-slot blob,
     * migrate it into the SIG slot, and persist v2. */
    pgp_key_t loaded[OPENPGP_SLOT_COUNT];
    memset(loaded, 0, sizeof(loaded));
    ver = 0;
    if (nvs_load_blob_with_total(STORAGE_NAMESPACE, "pgp_keys",
                                 loaded, sizeof(loaded),
                                 "pgp_keys_ver", &ver) == ESP_OK) {
        memcpy(s_keys, loaded, sizeof(s_keys));
    } else {
        typedef struct { uint8_t set; uint8_t algo; uint8_t d[32]; } pgp_key_v1_t;
        pgp_key_v1_t v1; memset(&v1, 0, sizeof(v1));
        ver = 0;
        if (nvs_load_blob_with_total(STORAGE_NAMESPACE, "pgp_key",
                                     &v1, sizeof(v1), "pgp_key_ver", &ver) == ESP_OK
            && v1.set) {
            s_keys[OPENPGP_SLOT_SIG].set    = 1;
            s_keys[OPENPGP_SLOT_SIG].algo   = v1.algo;
            s_keys[OPENPGP_SLOT_SIG].origin = 2;      /* Phase 1 = import only */
            memcpy(s_keys[OPENPGP_SLOT_SIG].d, v1.d, 32);
            if (key_persist())                         /* write v2 */
                ESP_LOGI(TAG, "migrated Phase-1 SIG key to multi-slot blob");
            else
                ESP_LOGW(TAG, "Phase-1 key migration: v2 persist failed, "
                              "will retry next boot");
        }
    }
}

#else /* TEST_HOST */

static bool key_persist(void) { return true; }
int  g_pin_persist_calls = 0;
static bool pin_persist(void) { g_pin_persist_calls++; return true; }
void openpgp_card_load(void) { /* host: no NVS */ }

#endif
