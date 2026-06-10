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

/* Algorithm attributes: 0x13=ECDSA, 0x12=ECDH; OID = NIST P-256 */
static const uint8_t FACTORY_C1[9] = {
    0x13, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};
static const uint8_t FACTORY_C2[9] = {
    0x12, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};
static const uint8_t FACTORY_C3[9] = {
    0x13, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};

/* UIF DOs: byte0 0x01=required, byte1 0x20=button-present.
 * Signing ON by default; decryption and authentication off. */
static const uint8_t FACTORY_D6[2] = {0x01, 0x20};
static const uint8_t FACTORY_D7[2] = {0x00, 0x20};
static const uint8_t FACTORY_D8[2] = {0x00, 0x20};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static const openpgp_card_hooks_t *s_hooks;

static bool    s_selected;
static bool    s_pw1_sign_verified;  /* mode 0x81 — required for PSO:CDS */
static bool    s_pw1_user_verified;  /* mode 0x82 — gate for PSO:DECIPHER (Phase 2) */
static bool    s_pw3_verified;
static uint8_t s_pw1_retry;          /* shared counter for both PW1 modes */
static uint8_t s_pw3_retry;

/* Mutable PINs (changeable via CHANGE REFERENCE DATA — Phase 2) */
static uint8_t s_pw1[PW_MAX_LEN];
static uint8_t s_pw1_len;
static uint8_t s_pw3[PW_MAX_LEN];
static uint8_t s_pw3_len;

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
    c4[0] = 0x00;          /* PW1 validity (0 = valid for one PSO:CDS) */
    c4[1] = PW_MAX_LEN;    /* max PW1 length */
    c4[2] = 0x00;          /* RFU / no resetting code */
    c4[3] = PW_MAX_LEN;    /* max PW3 length */
    c4[4] = s_pw1_retry;
    c4[5] = 0x00;          /* RC retries (not implemented) */
    c4[6] = s_pw3_retry;
}

/* Check UIF DO 0xD6: returns true if touch confirmation is required. */
static bool uif_signing_required(void)
{
    const uint8_t *v;
    uint16_t n;
    if (openpgp_do_get(0x00D6, &v, &n) && n >= 1 && v[0] != 0x00)
        return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Public API — lifecycle                                              */
/* ------------------------------------------------------------------ */

void openpgp_card_init(const openpgp_card_hooks_t *hooks)
{
    s_hooks = hooks;

    /* Reset session state */
    s_selected          = false;
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
    ENSURE(0x00C2, FACTORY_C2,    9);
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
    ENSURE(0x0093, NULL,          3);  /* DS counter: zero             */

#undef ENSURE

    return (failures == 0);
}

void openpgp_card_factory_reset(void)
{
    /* 1. Reset session state */
    s_selected          = false;
    s_pw1_sign_verified = false;
    s_pw1_user_verified = false;
    s_pw3_verified      = false;

    /* 2. Wipe DO store */
    openpgp_do_reset();

    /* 3. Restore factory PINs and retry counters */
    pins_factory();

    /* 4. Populate all factory-default DOs */
    openpgp_card_ensure_defaults();
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

/* ------------------------------------------------------------------ */
/* APDU dispatch                                                       */
/* ------------------------------------------------------------------ */

uint16_t openpgp_card_apdu(const uint8_t *in, uint16_t in_len,
                            uint8_t *out, uint16_t out_max)
{
    apdu_t a;
    if (!apdu_parse(in, in_len, &a))
        return sw_only(out, out_max, 0x6700); /* wrong length */

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

        /* PIN comparison */
        if (a.lc != pinlen || memcmp(a.data, pin, pinlen) != 0) {
            (*retry)--;
            return sw_only(out, out_max, (uint16_t)(0x63C0u | *retry));
        }

        /* Correct PIN — reset counter, set mode-specific verified flag */
        *retry = (is_pw1 ? PW1_RETRY_MAX : PW3_RETRY_MAX);
        if      (a.p2 == 0x81) s_pw1_sign_verified = true;
        else if (a.p2 == 0x82) s_pw1_user_verified = true;
        else                   s_pw3_verified       = true;
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
        if (tag == 0x006Eu) {
            uint8_t  body[256];
            uint16_t off = 0;

            /* 4F: AID */
            v = NULL; n = 0; openpgp_do_get(0x004F, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x004Fu, v, n);

            /* 5F52: Historical bytes (two-byte tag) */
            v = NULL; n = 0; openpgp_do_get(0x5F52u, &v, &n);
            APPEND_OR_FAIL(body, sizeof(body), off, 0x5F52u, v, n);

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

        /* Generic PUT DATA */
        if (!openpgp_do_put(tag, a.data, a.lc))
            return sw_only(out, out_max, SW_WRONG_DATA);
        return sw_only(out, out_max, SW_OK);
    }

    /* ---- PSO: Compute Digital Signature (INS=0x2A, P1=0x9E, P2=0x9A) ---- */
    if (a.ins == 0x2A) {
        if (a.p1 != 0x9E || a.p2 != 0x9A)
            return sw_only(out, out_max, SW_WRONG_P1P2);
        if (!s_pw1_sign_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);

        /* UIF gate: if DO 0xD6 byte[0] != 0, call confirm hook */
        if (uif_signing_required()) {
            int cs = s_hooks->confirm();
            if (cs != 1)
                return sw_only(out, out_max, SW_COND_NOT_SAT);
        }

        /* Invoke sign hook */
        uint8_t  sig[256];
        uint16_t sig_len = 0;
        if (!s_hooks->sign(a.data, a.lc, sig, &sig_len))
            return sw_only(out, out_max, SW_COND_NOT_SAT);
        return respond(out, out_max, sig, sig_len, SW_OK);
    }

    return sw_only(out, out_max, SW_INS_NOT_SUP);
}
