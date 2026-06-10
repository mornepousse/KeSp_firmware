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
/* State                                                               */
/* ------------------------------------------------------------------ */

static const openpgp_card_hooks_t *s_hooks;

static bool    s_selected;
static bool    s_pw1_sign_verified;  /* mode 0x81 — required for PSO:CDS */
static bool    s_pw1_user_verified;  /* mode 0x82 — required for decrypt etc. */
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
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void openpgp_card_init(const openpgp_card_hooks_t *hooks)
{
    s_hooks = hooks;

    /* Reset DO store */
    openpgp_do_reset();

    /* Reset applet state */
    s_selected          = false;
    s_pw1_sign_verified = false;
    s_pw1_user_verified = false;
    s_pw3_verified      = false;
    s_pw1_retry         = PW1_RETRY_MAX;
    s_pw3_retry         = PW3_RETRY_MAX;

    /* Restore factory-default PINs */
    memcpy(s_pw1, PW1_DEFAULT, PW1_DEFAULT_LEN);
    s_pw1_len = PW1_DEFAULT_LEN;
    memcpy(s_pw3, PW3_DEFAULT, PW3_DEFAULT_LEN);
    s_pw3_len = PW3_DEFAULT_LEN;
}

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

        /* Blocked? */
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

        /* Correct PIN — reset counter and set the mode-specific flag */
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
        if (!openpgp_do_get(tag, &v, &n))
            return sw_only(out, out_max, SW_REF_NOT_FOUND);
        return respond(out, out_max, v, n, SW_OK);
    }

    /* ---- PUT DATA (INS=0xDA) ---- */
    if (a.ins == 0xDA) {
        if (!s_pw3_verified)
            return sw_only(out, out_max, SW_SEC_NOT_SAT);
        uint16_t tag = ((uint16_t)a.p1 << 8) | a.p2;
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
