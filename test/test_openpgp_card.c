#include "test_framework.h"
#include "openpgp_card.h"
#include "openpgp_do.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fake hooks                                                          */
/* ------------------------------------------------------------------ */

static int g_confirm_retval = 1;   /* 0=pending, 1=authorized, 2=deny */

static bool fake_sign(const uint8_t *hash, uint16_t n,
                      uint8_t *out, uint16_t *out_n)
{
    (void)hash; (void)n;
    /* canned 32-byte signature */
    memset(out, 0x42, 32);
    *out_n = 32;
    return true;
}

static int fake_confirm(void) { return g_confirm_retval; }

/* ------------------------------------------------------------------ */
/* APDU builders                                                       */
/* ------------------------------------------------------------------ */

static const uint8_t OPGP_AID[] = {0xD2,0x76,0x00,0x01,0x24,0x01};

static uint16_t build_select(uint8_t *buf,
                             const uint8_t *aid, uint8_t aid_len)
{
    buf[0] = 0x00; buf[1] = 0xA4; buf[2] = 0x04; buf[3] = 0x00;
    buf[4] = aid_len;
    memcpy(buf + 5, aid, aid_len);
    return (uint16_t)(5 + aid_len);
}

static uint16_t build_verify(uint8_t p2,
                             const uint8_t *pin, uint8_t pin_len,
                             uint8_t *buf)
{
    buf[0] = 0x00; buf[1] = 0x20; buf[2] = 0x00; buf[3] = p2;
    buf[4] = pin_len;
    memcpy(buf + 5, pin, pin_len);
    return (uint16_t)(5 + pin_len);
}

static uint16_t build_pso_cds(const uint8_t *hash, uint8_t hash_len,
                               uint8_t *buf)
{
    buf[0] = 0x00; buf[1] = 0x2A; buf[2] = 0x9E; buf[3] = 0x9A;
    buf[4] = hash_len;
    memcpy(buf + 5, hash, hash_len);
    return (uint16_t)(5 + hash_len);
}

static uint16_t build_put_data(uint8_t p1, uint8_t p2,
                                const uint8_t *data, uint8_t data_len,
                                uint8_t *buf)
{
    buf[0] = 0x00; buf[1] = 0xDA; buf[2] = p1; buf[3] = p2;
    buf[4] = data_len;
    memcpy(buf + 5, data, data_len);
    return (uint16_t)(5 + data_len);
}

/* GET DATA — Case 2S: 00 CA P1 P2 00 */
static uint16_t build_get_data(uint8_t p1, uint8_t p2, uint8_t *buf)
{
    buf[0] = 0x00; buf[1] = 0xCA; buf[2] = p1; buf[3] = p2; buf[4] = 0x00;
    return 5;
}

/* Extract status word from the last 2 bytes of a response */
static uint16_t sw_of(const uint8_t *rsp, uint16_t len)
{
    if (len < 2) return 0x0000;
    return ((uint16_t)rsp[len - 2] << 8) | rsp[len - 1];
}

/* Search for needle[0..nlen-1] anywhere in hay[0..hlen-1] */
static bool has_bytes(const uint8_t *hay, uint16_t hlen,
                      const uint8_t *needle, uint16_t nlen)
{
    if (nlen == 0 || nlen > hlen) return false;
    for (uint16_t i = 0; (uint16_t)(i + nlen) <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Helper: drive SELECT, assert 9000                                   */
/* ------------------------------------------------------------------ */
static void do_select(uint8_t *cmd, uint8_t *rsp)
{
    uint16_t clen = build_select(cmd, OPGP_AID, sizeof(OPGP_AID));
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "SELECT returns 9000");
}

/* Helper: full card reset (hooks stored, DO store + PINs + DOs wiped to factory) */
static void setup_card(openpgp_card_hooks_t *h)
{
    openpgp_card_init(h);
    openpgp_card_factory_reset();
}

/* ------------------------------------------------------------------ */
/* Tests — existing (updated to use setup_card for PIN/DO state)       */
/* ------------------------------------------------------------------ */

static void test_select_ok(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen = build_select(cmd, OPGP_AID, sizeof(OPGP_AID));
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "SELECT OpenPGP AID returns 9000");

    /* Unknown AID must be rejected */
    uint8_t bad_aid[] = {0x00, 0x11, 0x22, 0x33};
    clen = build_select(cmd, bad_aid, sizeof(bad_aid));
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88, "bad AID rejected with 6A88");
}

static void test_sign_requires_pin(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];

    /* SELECT first */
    do_select(cmd, rsp);

    /* PSO:CDS before any VERIFY → 6982 security status not satisfied */
    uint8_t hash[20];
    memset(hash, 0xAB, 20);
    uint16_t clen = build_pso_cds(hash, 20, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982,
                   "PSO:CDS without VERIFY returns 6982");

    /* After VERIFY PW1 (mode 0x81 = signing mode) succeeds, signing works */
    uint8_t pw1[] = {'1','2','3','4','5','6'};
    clen = build_verify(0x81, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW1 (81) returns 9000");

    clen = build_pso_cds(hash, 20, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "PSO:CDS after VERIFY (UIF authorized) returns 9000");
}

static void test_sign_uif_gate(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* VERIFY PW3 (admin) to allow PUT DATA */
    uint8_t pw3[] = {'1','2','3','4','5','6','7','8'};
    clen = build_verify(0x83, pw3, sizeof(pw3), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW3 returns 9000");

    /* PUT DATA: enable UIF for signing (DO 0xD6, byte[0]=0x01)
     * factory default already has D6={0x01,0x20}; this is idempotent */
    uint8_t uif[] = {0x01, 0x20};
    clen = build_put_data(0x00, 0xD6, uif, sizeof(uif), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT DATA UIF returns 9000");

    /* VERIFY PW1 (mode 0x81 = signing mode) */
    uint8_t pw1[] = {'1','2','3','4','5','6'};
    clen = build_verify(0x81, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW1 (81) returns 9000");

    /* PSO:CDS with UIF enabled, confirm returns 1 (authorized) → 9000 */
    uint8_t hash[20];
    memset(hash, 0xBE, 20);
    clen = build_pso_cds(hash, 20, cmd);
    g_confirm_retval = 1;
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "PSO:CDS with UIF authorized returns 9000");

    /* Verify that the canned signature is in the response data */
    TEST_ASSERT(rlen >= 34, "response contains 32 sig bytes + SW");
    TEST_ASSERT_EQ(rsp[0], 0x42, "first sig byte from fake_sign");

    /* PSO:CDS with UIF enabled, confirm returns 2 (deny) → 6985 */
    clen = build_pso_cds(hash, 20, cmd);
    g_confirm_retval = 2;
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6985,
                   "PSO:CDS with UIF denied returns 6985");
}

static void test_pin_retry_counter(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* Three consecutive wrong PW1 attempts */
    uint8_t wrong[] = {'X','X','X','X','X','X'};
    clen = build_verify(0x82, wrong, sizeof(wrong), cmd);

    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "1st wrong PIN: 2 retries left");

    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C1, "2nd wrong PIN: 1 retry left");

    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C0, "3rd wrong PIN: 0 retries left");

    /* Correct PIN is now blocked */
    uint8_t correct[] = {'1','2','3','4','5','6'};
    clen = build_verify(0x82, correct, sizeof(correct), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6983,
                   "blocked PIN returns 6983");
}

/* VERIFY P2=0x81 (PW1 for signing) is accepted and gates PSO:CDS;
   P2=0x82 alone must NOT open the signing gate. */
static void test_verify_81_gates_sign(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;
    uint8_t hash[20];
    memset(hash, 0xCC, 20);
    uint8_t pw1[] = {'1','2','3','4','5','6'};

    do_select(cmd, rsp);

    /* VERIFY 0x82 alone must NOT satisfy the signing gate */
    clen = build_verify(0x82, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY 82 ok");

    clen = build_pso_cds(hash, sizeof(hash), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982, "PSO:CDS still gated after 82 only");

    /* VERIFY 0x81 satisfies the signing gate */
    clen = build_verify(0x81, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY 81 ok");

    clen = build_pso_cds(hash, sizeof(hash), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PSO:CDS passes after 81");
}

/* Wrong PIN on P2=0x81 and P2=0x82 decrement the SAME PW1 retry counter. */
static void test_verify_81_82_share_retries(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;
    uint8_t pw1[] = {'1','2','3','4','5','6'};

    do_select(cmd, rsp);

    /* Wrong PIN on 0x81: counter 3→2 */
    clen = build_verify(0x81, (const uint8_t *)"000000", 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "81 wrong -> 2 left");

    /* Wrong PIN on 0x82: counter 2→1 (shared) */
    clen = build_verify(0x82, (const uint8_t *)"000000", 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C1, "82 wrong -> 1 left (shared)");

    /* Correct PIN (on either mode) resets shared counter back to max */
    clen = build_verify(0x81, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "81 correct after wrong attempts");
    uint8_t check[5] = {0x00, 0x20, 0x00, 0x81, 0x00};
    rlen = openpgp_card_apdu(check, 5, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C3, "counter reset to 3 after correct PIN");
}

/* ------------------------------------------------------------------ */
/* Tests — new (Task 3: factory DOs + constructed DOs)                 */
/* ------------------------------------------------------------------ */

/* factory_reset + SELECT; GET DATA 4F → 9000, 16 bytes, D2 76 prefix,
   version 03 04 at offsets 6-7. */
static void test_factory_defaults_aid(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x00, 0x4F, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 4F returns 9000");
    TEST_ASSERT_EQ(rlen - 2, 16, "AID is 16 bytes");
    TEST_ASSERT_EQ(rsp[0], 0xD2, "AID byte[0] = D2");
    TEST_ASSERT_EQ(rsp[1], 0x76, "AID byte[1] = 76");
    TEST_ASSERT_EQ(rsp[6], 0x03, "AID version byte[6] = 03");
    TEST_ASSERT_EQ(rsp[7], 0x04, "AID version byte[7] = 04");
}

/* GET DATA 6E → 9000; body length in [200,254); starts with 4F 10 D2;
   contains C5 3C header; contains synthesised C4 TLV with live retries. */
static void test_get_data_6e_constructed(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x00, 0x6E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 6E returns 9000");

    uint16_t dlen = (uint16_t)(rlen - 2);
    TEST_ASSERT(dlen >= 200 && dlen < 254, "6E data length in [200,254)");

    /* Response body must start with 4F 10 D2 (4F TLV of 16-byte AID) */
    TEST_ASSERT_EQ(rsp[0], 0x4F, "6E body[0] = 4F");
    TEST_ASSERT_EQ(rsp[1], 0x10, "6E body[1] = 10 (AID len=16)");
    TEST_ASSERT_EQ(rsp[2], 0xD2, "6E body[2] = D2 (AID first byte)");

    /* Must contain C5 TLV header: tag 0xC5, len 0x3C (60 decimal) */
    uint8_t c5_hdr[] = {0xC5, 0x3C};
    TEST_ASSERT(has_bytes(rsp, dlen, c5_hdr, 2), "6E contains C5 3C header");

    /* Must contain synthesised C4 TLV: C4 07 00 10 00 10 03 00 03
       (pw1_retry=3, pw3_retry=3 at factory-reset state) */
    uint8_t c4_factory[] = {0xC4, 0x07, 0x00, 0x10, 0x00, 0x10, 0x03, 0x00, 0x03};
    TEST_ASSERT(has_bytes(rsp, dlen, c4_factory, 9),
                "6E contains synthesised C4 TLV (full counters)");

    /* C4 must reflect live retry counters: fail one PW1 attempt then re-read */
    uint8_t wrong[] = {'X','X','X','X','X','X'};
    uint16_t vclen = build_verify(0x81, wrong, 6, cmd);
    openpgp_card_apdu(cmd, vclen, rsp, sizeof(rsp));

    clen = build_get_data(0x00, 0x6E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "GET DATA 6E after failed verify still returns 9000");
    dlen = (uint16_t)(rlen - 2);
    /* pw1_retry decremented to 2, pw3_retry still 3 */
    uint8_t c4_after[] = {0xC4, 0x07, 0x00, 0x10, 0x00, 0x10, 0x02, 0x00, 0x03};
    TEST_ASSERT(has_bytes(rsp, dlen, c4_after, 9),
                "C4 in 6E reflects decremented PW1 retry counter");
}

/* PW3 verify; PUT DATA C7 (20×0xAB) → 9000; GET DATA C5 → 60 B,
   [0..19]=0xAB, [20]=0x00.  PUT DATA C7 with 19 B → 6A80. */
static void test_fingerprint_slices(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* VERIFY PW3 */
    uint8_t pw3[] = {'1','2','3','4','5','6','7','8'};
    clen = build_verify(0x83, pw3, sizeof(pw3), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW3 returns 9000");

    /* PUT DATA C7 (20 × 0xAB) → 9000 */
    uint8_t fp20[20];
    memset(fp20, 0xAB, 20);
    clen = build_put_data(0x00, 0xC7, fp20, 20, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT DATA C7 (20B) returns 9000");

    /* GET DATA C5 → 60 bytes; [0..19]=0xAB, [20]=0x00 */
    clen = build_get_data(0x00, 0xC5, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA C5 returns 9000");
    TEST_ASSERT_EQ(rlen - 2, 60, "C5 is 60 bytes");
    TEST_ASSERT_EQ(rsp[0],  0xAB, "C5[0] = 0xAB (C7 slice written)");
    TEST_ASSERT_EQ(rsp[19], 0xAB, "C5[19] = 0xAB (end of C7 slice)");
    TEST_ASSERT_EQ(rsp[20], 0x00, "C5[20] = 0x00 (next slot untouched)");

    /* PUT DATA C7 with 19 bytes → 6A80 (wrong length) */
    uint8_t fp19[19];
    memset(fp19, 0xCC, 19);
    clen = build_put_data(0x00, 0xC7, fp19, 19, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "PUT DATA C7 (19B) → 6A80");
}

/* GET DATA 65 → 9000; GET DATA 7A → 9000 with body 93 03 00 00 00. */
static void test_get_data_65_7a(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* GET DATA 65 (Cardholder Related Data) → 9000 */
    clen = build_get_data(0x00, 0x65, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 65 returns 9000");

    /* GET DATA 7A (Security support template) → 9000, body = 93 03 00 00 00 */
    clen = build_get_data(0x00, 0x7A, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 7A returns 9000");
    TEST_ASSERT_EQ(rlen - 2, 5, "7A body is 5 bytes");
    TEST_ASSERT_EQ(rsp[0], 0x93, "7A body[0] = 93 (DS counter tag)");
    TEST_ASSERT_EQ(rsp[1], 0x03, "7A body[1] = 03 (len)");
    TEST_ASSERT_EQ(rsp[2], 0x00, "7A DS counter byte 0 = 0");
    TEST_ASSERT_EQ(rsp[3], 0x00, "7A DS counter byte 1 = 0");
    TEST_ASSERT_EQ(rsp[4], 0x00, "7A DS counter byte 2 = 0");
}

/* After factory_reset (17 factory DOs), one more DO must succeed,
   proving MAX_ENTRIES was bumped beyond 16. */
static void test_do_capacity(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    /* factory_reset stored 17 DOs; add a custom DO as the 18th */
    uint8_t extra[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT(openpgp_do_put(0x0101, extra, 4),
                "18th DO put succeeds (MAX_ENTRIES >= 18)");

    const uint8_t *v;
    uint16_t n;
    TEST_ASSERT(openpgp_do_get(0x0101, &v, &n), "18th DO get succeeds");
    TEST_ASSERT_EQ(n, 4, "18th DO has correct length");
}

/* set_serial patches bytes [10..13] of the AID without disturbing the prefix. */
static void test_set_serial(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    uint8_t serial[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    openpgp_card_set_serial(serial);

    /* GET DATA 4F → 16 bytes; prefix intact; serial at [10..13] */
    clen = build_get_data(0x00, 0x4F, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 4F after set_serial → 9000");
    TEST_ASSERT_EQ(rlen - 2, 16, "AID still 16 bytes");
    TEST_ASSERT_EQ(rsp[0],  0xD2, "AID prefix D2 intact");
    TEST_ASSERT_EQ(rsp[1],  0x76, "AID prefix 76 intact");
    TEST_ASSERT_EQ(rsp[10], 0xDE, "AID serial[0] = DE");
    TEST_ASSERT_EQ(rsp[11], 0xAD, "AID serial[1] = AD");
    TEST_ASSERT_EQ(rsp[12], 0xBE, "AID serial[2] = BE");
    TEST_ASSERT_EQ(rsp[13], 0xEF, "AID serial[3] = EF");
}

void test_openpgp_card(void)
{
    TEST_SUITE("openpgp applet");
    TEST_RUN(test_select_ok);
    TEST_RUN(test_sign_requires_pin);
    TEST_RUN(test_sign_uif_gate);
    TEST_RUN(test_pin_retry_counter);
    TEST_RUN(test_verify_81_gates_sign);
    TEST_RUN(test_verify_81_82_share_retries);
    /* Task 3: factory DOs + constructed DOs */
    TEST_RUN(test_factory_defaults_aid);
    TEST_RUN(test_get_data_6e_constructed);
    TEST_RUN(test_fingerprint_slices);
    TEST_RUN(test_get_data_65_7a);
    TEST_RUN(test_do_capacity);
    TEST_RUN(test_set_serial);
}
