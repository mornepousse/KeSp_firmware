#include "test_framework.h"
#include "openpgp_card.h"
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

/* Extract status word from the last 2 bytes of a response */
static uint16_t sw_of(const uint8_t *rsp, uint16_t len)
{
    if (len < 2) return 0x0000;
    return ((uint16_t)rsp[len - 2] << 8) | rsp[len - 1];
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

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_select_ok(void)
{
    openpgp_card_hooks_t h = { fake_sign, fake_confirm };
    openpgp_card_init(&h);
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
    openpgp_card_hooks_t h = { fake_sign, fake_confirm };
    openpgp_card_init(&h);
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

    /* After VERIFY PW1 succeeds, signing works (no UIF) */
    uint8_t pw1[] = {'1','2','3','4','5','6'};
    clen = build_verify(0x82, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW1 returns 9000");

    clen = build_pso_cds(hash, 20, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "PSO:CDS after VERIFY (no UIF) returns 9000");
}

static void test_sign_uif_gate(void)
{
    openpgp_card_hooks_t h = { fake_sign, fake_confirm };
    openpgp_card_init(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* VERIFY PW3 (admin) to allow PUT DATA */
    uint8_t pw3[] = {'1','2','3','4','5','6','7','8'};
    clen = build_verify(0x83, pw3, sizeof(pw3), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW3 returns 9000");

    /* PUT DATA: enable UIF for signing (DO 0xD6, byte[0]=0x01) */
    uint8_t uif[] = {0x01, 0x20};
    clen = build_put_data(0x00, 0xD6, uif, sizeof(uif), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT DATA UIF returns 9000");

    /* VERIFY PW1 */
    uint8_t pw1[] = {'1','2','3','4','5','6'};
    clen = build_verify(0x82, pw1, sizeof(pw1), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW1 returns 9000");

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
    openpgp_card_hooks_t h = { fake_sign, fake_confirm };
    openpgp_card_init(&h);
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

void test_openpgp_card(void)
{
    TEST_SUITE("openpgp applet");
    TEST_RUN(test_select_ok);
    TEST_RUN(test_sign_requires_pin);
    TEST_RUN(test_sign_uif_gate);
    TEST_RUN(test_pin_retry_counter);
}
