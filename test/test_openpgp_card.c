#include "test_framework.h"
#include "openpgp_card.h"
#include "openpgp_do.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fake hooks                                                          */
/* ------------------------------------------------------------------ */

static int      g_confirm_retval      = 1;   /* 0=pending, 1=authorized, 2=deny */
static uint8_t  g_fake_sign_last_d[32];      /* records d from last sign call    */

static bool fake_sign(const uint8_t d[32],
                      const uint8_t *hash, uint16_t n,
                      uint8_t *out, uint16_t *out_n)
{
    memcpy(g_fake_sign_last_d, d, 32);
    (void)hash; (void)n;
    /* canned 32-byte signature */
    memset(out, 0x42, 32);
    *out_n = 32;
    return true;
}

/* Sign hook that always fails — used by test_sign_consumes_pw1_on_failure. */
static bool fake_sign_fail(const uint8_t d[32],
                           const uint8_t *hash, uint16_t n,
                           uint8_t *out, uint16_t *out_n)
{
    (void)d; (void)hash; (void)n; (void)out; (void)out_n;
    return false;
}

static int fake_confirm(void) { return g_confirm_retval; }

/* Deterministic fake pubkey: 0x04 || d[32] || d[32] (easy to verify in tests). */
static bool fake_pubkey(const uint8_t d[32], uint8_t out_pub[65])
{
    out_pub[0] = 0x04;
    memcpy(out_pub + 1,  d, 32);
    memcpy(out_pub + 33, d, 32);
    return true;
}

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

/* CHANGE REFERENCE DATA — 00 24 00 P2 lc old_pin||new_pin */
static uint16_t build_change_ref(uint8_t p2,
                                  const uint8_t *old_pin, uint8_t old_len,
                                  const uint8_t *new_pin, uint8_t new_len,
                                  uint8_t *buf)
{
    uint8_t lc = (uint8_t)(old_len + new_len);
    buf[0] = 0x00; buf[1] = 0x24; buf[2] = 0x00; buf[3] = p2;
    buf[4] = lc;
    memcpy(buf + 5,            old_pin, old_len);
    memcpy(buf + 5 + old_len,  new_pin, new_len);
    return (uint16_t)(5 + lc);
}

/*
 * Minimal key-import APDU: 00 DB 3F FF 2C 4D 2A B6 00 7F48 02 92 20 5F48 20 d[32]
 *
 * Inner content of 4D (42 bytes):
 *   B6 00            — sig-slot CRT, empty
 *   7F 48 02 92 20   — template: priv-key (92) is 32 bytes
 *   5F 48 20 d[32]   — 32-byte key material
 */
static uint16_t build_key_import(const uint8_t d[32], uint8_t *buf)
{
    buf[0]  = 0x00; buf[1]  = 0xDB; buf[2]  = 0x3F; buf[3]  = 0xFF;
    buf[4]  = 0x2Cu; /* Lc = 44 bytes of data */
    buf[5]  = 0x4Du; buf[6]  = 0x2Au; /* outer tag 4D, inner len = 42 */
    buf[7]  = 0xB6u; buf[8]  = 0x00u; /* B6 00: sig-slot CRT */
    buf[9]  = 0x7Fu; buf[10] = 0x48u; buf[11] = 0x02u; /* 7F48 tag + len */
    buf[12] = 0x92u; buf[13] = 0x20u; /* element: priv-key, 32 B */
    buf[14] = 0x5Fu; buf[15] = 0x48u; buf[16] = 0x20u; /* 5F48 tag + len */
    memcpy(buf + 17, d, 32);
    return 49; /* 5 header + 44 data */
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

/* Return byte-offset of first occurrence of needle in hay, or -1 if absent. */
static int find_index(const uint8_t *hay, uint16_t hlen,
                      const uint8_t *needle, uint16_t nlen)
{
    if (nlen == 0 || nlen > hlen) return -1;
    for (uint16_t i = 0; (uint16_t)(i + nlen) <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return (int)i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Shared test helpers                                                 */
/* ------------------------------------------------------------------ */

static void do_select(uint8_t *cmd, uint8_t *rsp)
{
    uint16_t clen = build_select(cmd, OPGP_AID, sizeof(OPGP_AID));
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "SELECT returns 9000");
}

static void setup_card(openpgp_card_hooks_t *h)
{
    /* Reset globals so every test starts from a known state (order-independent). */
    g_confirm_retval = 1;
    memset(g_fake_sign_last_d, 0, sizeof(g_fake_sign_last_d));
    /* Ensure the crypto health flag is true regardless of test ordering. */
    openpgp_card_set_crypto_health(true);
    openpgp_card_init(h);
    TEST_ASSERT(openpgp_card_factory_reset(), "factory_reset succeeds");
}

static void do_verify_pw3(uint8_t *cmd, uint8_t *rsp)
{
    static const uint8_t pw3[] = {'1','2','3','4','5','6','7','8'};
    uint16_t clen = build_verify(0x83, pw3, sizeof(pw3), cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW3 returns 9000");
}

static void do_verify_pw1_sign(uint8_t *cmd, uint8_t *rsp)
{
    static const uint8_t pw1[] = {'1','2','3','4','5','6'};
    uint16_t clen = build_verify(0x81, pw1, sizeof(pw1), cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "VERIFY PW1 (81) returns 9000");
}

/* Import with the standard B6-00 format; PW3 must already be verified. */
static void do_import_key(const uint8_t d[32], uint8_t *cmd, uint8_t *rsp)
{
    uint16_t clen = build_key_import(d, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "key import returns 9000");
}

/* Disable UIF for signing; PW3 must already be verified. */
static void do_disable_uif(uint8_t *cmd, uint8_t *rsp)
{
    static const uint8_t uif_off[] = {0x00, 0x20};
    uint16_t clen = build_put_data(0x00, 0xD6, uif_off, sizeof(uif_off), cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, 256);
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT DATA D6 disable UIF returns 9000");
}

/* ------------------------------------------------------------------ */
/* Tests — existing (updated to work with key-present requirement)     */
/* ------------------------------------------------------------------ */

static void test_select_ok(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
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
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];

    do_select(cmd, rsp);

    /* Import a key first (PW3 required) so PSO:CDS isn't gated by 6A88 */
    do_verify_pw3(cmd, rsp);
    static const uint8_t key_d[32] = {0xAA};
    do_import_key(key_d, cmd, rsp);

    /* PSO:CDS before PW1 VERIFY → 6982 (key set, PW1 not verified) */
    uint8_t hash[20];
    memset(hash, 0xAB, 20);
    uint16_t clen = build_pso_cds(hash, 20, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982,
                   "PSO:CDS without VERIFY returns 6982");

    /* After VERIFY PW1 (mode 0x81 = signing mode) succeeds, signing works */
    do_verify_pw1_sign(cmd, rsp);

    clen = build_pso_cds(hash, 20, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "PSO:CDS after VERIFY (UIF authorized) returns 9000");
}

static void test_sign_uif_gate(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* VERIFY PW3 (admin) needed for key import and PUT DATA */
    do_verify_pw3(cmd, rsp);

    /* Import key so PSO:CDS can proceed past the 6A88 gate */
    static const uint8_t key_d[32] = {0xBB};
    do_import_key(key_d, cmd, rsp);

    /* PUT DATA: enable UIF for signing (DO 0xD6, byte[0]=0x01)
     * factory default already has D6={0x01,0x20}; this is idempotent */
    uint8_t uif[] = {0x01, 0x20};
    clen = build_put_data(0x00, 0xD6, uif, sizeof(uif), cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT DATA UIF returns 9000");

    /* VERIFY PW1 (mode 0x81 = signing mode) */
    do_verify_pw1_sign(cmd, rsp);

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

    /* PW1 was consumed by the successful sign; re-verify before second attempt */
    do_verify_pw1_sign(cmd, rsp);

    /* PSO:CDS with UIF enabled, confirm returns 2 (deny) → 6985 */
    clen = build_pso_cds(hash, 20, cmd);
    g_confirm_retval = 2;
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6985,
                   "PSO:CDS with UIF denied returns 6985");
}

static void test_pin_retry_counter(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
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
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;
    uint8_t hash[20];
    memset(hash, 0xCC, 20);

    do_select(cmd, rsp);

    /* Import a key so PSO:CDS is gated by PIN, not by missing key */
    do_verify_pw3(cmd, rsp);
    static const uint8_t key_d[32] = {0xCC};
    do_import_key(key_d, cmd, rsp);

    /* VERIFY 0x82 alone must NOT satisfy the signing gate */
    uint8_t pw1[] = {'1','2','3','4','5','6'};
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
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
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
/* Tests — Task 3: factory DOs + constructed DOs                      */
/* ------------------------------------------------------------------ */

static void test_factory_defaults_aid(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
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

static void test_get_data_6e_constructed(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x00, 0x6E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 6E returns 9000");

    uint16_t dlen = (uint16_t)(rlen - 2);
    TEST_ASSERT(dlen >= 200 && dlen < 254, "6E data length in [200,254)");

    TEST_ASSERT_EQ(rsp[0], 0x4F, "6E body[0] = 4F");
    TEST_ASSERT_EQ(rsp[1], 0x10, "6E body[1] = 10 (AID len=16)");
    TEST_ASSERT_EQ(rsp[2], 0xD2, "6E body[2] = D2 (AID first byte)");

    uint8_t c5_hdr[] = {0xC5, 0x3C};
    TEST_ASSERT(has_bytes(rsp, dlen, c5_hdr, 2), "6E contains C5 3C header");

    uint8_t c4_factory[] = {0xC4, 0x07, 0x00, 0x10, 0x00, 0x10, 0x03, 0x00, 0x03};
    TEST_ASSERT(has_bytes(rsp, dlen, c4_factory, 9),
                "6E contains synthesised C4 TLV (full counters)");

    uint8_t wrong[] = {'X','X','X','X','X','X'};
    uint16_t vclen = build_verify(0x81, wrong, 6, cmd);
    openpgp_card_apdu(cmd, vclen, rsp, sizeof(rsp));

    clen = build_get_data(0x00, 0x6E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "GET DATA 6E after failed verify still returns 9000");
    dlen = (uint16_t)(rlen - 2);
    uint8_t c4_after[] = {0xC4, 0x07, 0x00, 0x10, 0x00, 0x10, 0x02, 0x00, 0x03};
    TEST_ASSERT(has_bytes(rsp, dlen, c4_after, 9),
                "C4 in 6E reflects decremented PW1 retry counter");
}

static void test_fingerprint_slices(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    uint8_t fp20[20];
    memset(fp20, 0xAB, 20);
    clen = build_put_data(0x00, 0xC7, fp20, 20, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PUT DATA C7 (20B) returns 9000");

    clen = build_get_data(0x00, 0xC5, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA C5 returns 9000");
    TEST_ASSERT_EQ(rlen - 2, 60, "C5 is 60 bytes");
    TEST_ASSERT_EQ(rsp[0],  0xAB, "C5[0] = 0xAB (C7 slice written)");
    TEST_ASSERT_EQ(rsp[19], 0xAB, "C5[19] = 0xAB (end of C7 slice)");
    TEST_ASSERT_EQ(rsp[20], 0x00, "C5[20] = 0x00 (next slot untouched)");

    uint8_t fp19[19];
    memset(fp19, 0xCC, 19);
    clen = build_put_data(0x00, 0xC7, fp19, 19, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "PUT DATA C7 (19B) → 6A80");
}

static void test_get_data_65_7a(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x00, 0x65, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 65 returns 9000");

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

/* ------------------------------------------------------------------ */
/* Tests — HW-validated fix: 5E login data + 7F74 general feature mgmt */
/* ------------------------------------------------------------------ */

static void test_login_data_empty(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[8], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x00, 0x5E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 5E returns 9000 (not 6A88)");
    TEST_ASSERT_EQ(rlen - 2, 0, "5E data length is 0 bytes (empty)");
}

static void test_gf_management(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[8], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x7F, 0x74, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 7F74 returns 9000");
    TEST_ASSERT_EQ(rlen - 2, 3, "7F74 data length is 3 bytes");
    TEST_ASSERT_EQ(rsp[0], 0x81, "7F74 byte[0] = 0x81 (template tag)");
    TEST_ASSERT_EQ(rsp[1], 0x01, "7F74 byte[1] = 0x01 (len)");
    TEST_ASSERT_EQ(rsp[2], 0x20, "7F74 byte[2] = 0x20 (button present)");
}

static void test_6e_contains_7f74(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[8], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_get_data(0x00, 0x6E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 6E returns 9000");

    uint16_t dlen = (uint16_t)(rlen - 2);

    uint8_t gfm[] = {0x7F, 0x74, 0x03, 0x81, 0x01, 0x20};
    TEST_ASSERT(has_bytes(rsp, dlen, gfm, 6), "6E contains 7F 74 03 81 01 20");

    uint8_t hist_hdr[]  = {0x5F, 0x52};
    uint8_t disc_hdr[]  = {0x73, 0x81};
    int idx_5f52 = find_index(rsp, dlen, hist_hdr, 2);
    int idx_7f74 = find_index(rsp, dlen, gfm,      6);
    int idx_73   = find_index(rsp, dlen, disc_hdr,  2);

    TEST_ASSERT(idx_5f52 >= 0, "5F52 TLV present in 6E");
    TEST_ASSERT(idx_7f74 >= 0, "7F74 TLV present in 6E");
    TEST_ASSERT(idx_73   >= 0, "73 discretionary tag present in 6E");
    TEST_ASSERT(idx_7f74 > idx_5f52, "7F74 appears after 5F52");
    TEST_ASSERT(idx_7f74 < idx_73,   "7F74 appears before 73");
}

/* ------------------------------------------------------------------ */
/* Tests — DO store capacity                                           */
/* ------------------------------------------------------------------ */

static void test_do_capacity(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t extra[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT(openpgp_do_put(0x0101, extra, 4),
                "20th DO put succeeds (MAX_ENTRIES >= 20)");

    const uint8_t *v;
    uint16_t n;
    TEST_ASSERT(openpgp_do_get(0x0101, &v, &n), "18th DO get succeeds");
    TEST_ASSERT_EQ(n, 4, "18th DO has correct length");
}

static void test_set_serial(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    uint8_t serial[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    openpgp_card_set_serial(serial);

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

/* ------------------------------------------------------------------ */
/* Tests — Task 5: key import (PUT DATA 0xDB 3FFF) + DS counter       */
/* ------------------------------------------------------------------ */

/* Key import without PW3 → 6982; key not set afterwards. */
static void test_key_import_requires_pw3(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    /* Do NOT verify PW3 */

    static const uint8_t d[32] = {0x11};
    uint16_t clen = build_key_import(d, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982,
                   "key import without PW3 returns 6982");
    TEST_ASSERT(!openpgp_card_key_is_set(), "key not set after rejected import");
}

/* Full happy-path: PW3 → import → key_is_set; VERIFY 81 + UIF off → PSO:CDS 9000;
   fake_sign received the correct private scalar. */
static void test_key_import_ok_and_sign_uses_it(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* Distinctive private key */
    uint8_t d[32];
    for (int i = 0; i < 32; i++) d[i] = (uint8_t)(0x10 + i);

    do_import_key(d, cmd, rsp);
    TEST_ASSERT(openpgp_card_key_is_set(), "key_is_set after successful import");

    /* Disable UIF for a clean sign test */
    do_disable_uif(cmd, rsp);

    do_verify_pw1_sign(cmd, rsp);

    uint8_t hash[32];
    memset(hash, 0xDE, 32);
    uint16_t clen = build_pso_cds(hash, 32, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "PSO:CDS with imported key returns 9000");

    TEST_ASSERT(memcmp(g_fake_sign_last_d, d, 32) == 0,
                "fake_sign received the imported private scalar");
}

/* B6 with a 3-byte reference value (84 01 01) must also be accepted. */
static void test_key_import_b6_variant(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* Build APDU manually: B6 03 84 01 01 instead of B6 00 */
    static const uint8_t d[32] = {0x22};
    uint8_t inner[64];
    uint16_t ii = 0;
    inner[ii++]=0xB6; inner[ii++]=0x03;
    inner[ii++]=0x84; inner[ii++]=0x01; inner[ii++]=0x01; /* B6 with ref */
    inner[ii++]=0x7F; inner[ii++]=0x48; inner[ii++]=0x02; /* 7F48 tag+len */
    inner[ii++]=0x92; inner[ii++]=0x20;                   /* priv-key, 32 B */
    inner[ii++]=0x5F; inner[ii++]=0x48; inner[ii++]=0x20; /* 5F48 tag+len */
    memcpy(inner + ii, d, 32); ii += 32;

    cmd[0]=0x00; cmd[1]=0xDB; cmd[2]=0x3F; cmd[3]=0xFF;
    cmd[4]=(uint8_t)(2u + ii);  /* Lc */
    cmd[5]=0x4D; cmd[6]=(uint8_t)ii;
    memcpy(cmd + 7, inner, ii);
    uint16_t clen = (uint16_t)(7u + ii);

    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "key import with B6 03 84 01 01 returns 9000");
    TEST_ASSERT(openpgp_card_key_is_set(), "key_is_set after B6-variant import");
}

/* B8 (decrypt slot) instead of B6 (sig slot) → 6A80; key not set. */
static void test_key_import_wrong_slot_b8(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* Same as build_key_import but with B8 instead of B6 */
    static const uint8_t d[32] = {0x33};
    uint16_t clen = build_key_import(d, cmd);
    cmd[7] = 0xB8u; /* overwrite B6 tag with B8 */

    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80,
                   "key import with B8 (decrypt slot) returns 6A80");
    TEST_ASSERT(!openpgp_card_key_is_set(), "key not set after B8 rejection");
}

/* 92 with length 16 (not 32) → 6A80 */
static void test_key_import_bad_92_len(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* Build APDU: template says 92 10 (16 bytes, wrong) */
    static const uint8_t d16[16] = {0x44};
    uint8_t inner[64];
    uint16_t ii = 0;
    inner[ii++]=0xB6; inner[ii++]=0x00;
    inner[ii++]=0x7F; inner[ii++]=0x48; inner[ii++]=0x02;
    inner[ii++]=0x92; inner[ii++]=0x10; /* 16 bytes — wrong */
    inner[ii++]=0x5F; inner[ii++]=0x48; inner[ii++]=0x10; /* 16 B payload */
    memcpy(inner + ii, d16, 16); ii += 16;

    cmd[0]=0x00; cmd[1]=0xDB; cmd[2]=0x3F; cmd[3]=0xFF;
    cmd[4]=(uint8_t)(2u + ii);
    cmd[5]=0x4D; cmd[6]=(uint8_t)ii;
    memcpy(cmd + 7, inner, ii);
    uint16_t clen = (uint16_t)(7u + ii);

    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80,
                   "key import with 92 len=16 returns 6A80");
}

/* Fresh card (no key), PW1 verified, UIF off → PSO:CDS returns 6A88. */
static void test_sign_without_key_6a88(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);

    /* Disable UIF (PW3 required) */
    do_verify_pw3(cmd, rsp);
    do_disable_uif(cmd, rsp);

    /* Verify PW1 sign */
    do_verify_pw1_sign(cmd, rsp);

    /* PSO:CDS with PW1 verified but no key → 6A88 */
    uint8_t hash[32]; memset(hash, 0xEE, 32);
    uint16_t clen = build_pso_cds(hash, 32, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88,
                   "PSO:CDS without imported key returns 6A88");
}

/* After one successful PSO:CDS, a second without re-VERIFY → 6982 (PW1 consumed). */
static void test_sign_consumes_pw1(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0x55};
    do_import_key(d, cmd, rsp);
    do_disable_uif(cmd, rsp);
    do_verify_pw1_sign(cmd, rsp);

    uint8_t hash[32]; memset(hash, 0xAA, 32);
    uint16_t clen = build_pso_cds(hash, 32, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "first PSO:CDS returns 9000");

    /* PW1 consumed — second PSO:CDS without re-VERIFY → 6982 */
    clen = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982,
                   "second PSO:CDS without re-VERIFY returns 6982");
}

/* Two successful signs (each with its own VERIFY 81) → DS counter = 2. */
static void test_ds_counter_increments(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0x66};
    do_import_key(d, cmd, rsp);
    do_disable_uif(cmd, rsp);

    uint8_t hash[32]; memset(hash, 0xBB, 32);

    /* First sign */
    do_verify_pw1_sign(cmd, rsp);
    uint16_t clen = build_pso_cds(hash, 32, cmd);
    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "first sign returns 9000");

    /* Second sign */
    do_verify_pw1_sign(cmd, rsp);
    clen = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "second sign returns 9000");

    /* GET DATA 7A → Security Support Template: 93 03 00 00 02 */
    clen = build_get_data(0x00, 0x7A, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "GET DATA 7A returns 9000");
    TEST_ASSERT_EQ(rlen - 2, 5, "7A body is 5 bytes");
    TEST_ASSERT_EQ(rsp[0], 0x93, "7A[0] = 93");
    TEST_ASSERT_EQ(rsp[1], 0x03, "7A[1] = 03 (len)");
    TEST_ASSERT_EQ(rsp[2], 0x00, "DS counter[0] = 0");
    TEST_ASSERT_EQ(rsp[3], 0x00, "DS counter[1] = 0");
    TEST_ASSERT_EQ(rsp[4], 0x02, "DS counter[2] = 2");
}

/* ------------------------------------------------------------------ */
/* Tests — W4: negative key-import paths                               */
/* ------------------------------------------------------------------ */

/* 7F48 template absent → 6A80; key not set. */
static void test_key_import_7f48_absent(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* Build: B6 00 (sig CRT) + 5F48 payload — no 7F48 template. */
    static const uint8_t d[32] = {0x77};
    uint8_t inner[64]; uint16_t ii = 0;
    inner[ii++]=0xB6; inner[ii++]=0x00;
    inner[ii++]=0x5F; inner[ii++]=0x48; inner[ii++]=0x20;
    memcpy(inner + ii, d, 32); ii += 32;

    cmd[0]=0x00; cmd[1]=0xDB; cmd[2]=0x3F; cmd[3]=0xFF;
    cmd[4]=(uint8_t)(2u + ii); cmd[5]=0x4D; cmd[6]=(uint8_t)ii;
    memcpy(cmd + 7, inner, ii);
    uint16_t clen = (uint16_t)(7u + ii);

    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80,
                   "key import with 7F48 absent returns 6A80");
    TEST_ASSERT(!openpgp_card_key_is_set(), "key not set after 7F48-absent rejection");
}

/* 5F48 key material absent → 6A80; key not set. */
static void test_key_import_5f48_absent(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* Build: B6 00 + 7F48 template — no 5F48. */
    uint8_t inner[64]; uint16_t ii = 0;
    inner[ii++]=0xB6; inner[ii++]=0x00;
    inner[ii++]=0x7F; inner[ii++]=0x48; inner[ii++]=0x02;
    inner[ii++]=0x92; inner[ii++]=0x20; /* declares 32 B for element 92 */
    /* no 5F48 */

    cmd[0]=0x00; cmd[1]=0xDB; cmd[2]=0x3F; cmd[3]=0xFF;
    cmd[4]=(uint8_t)(2u + ii); cmd[5]=0x4D; cmd[6]=(uint8_t)ii;
    memcpy(cmd + 7, inner, ii);
    uint16_t clen = (uint16_t)(7u + ii);

    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80,
                   "key import with 5F48 absent returns 6A80");
    TEST_ASSERT(!openpgp_card_key_is_set(), "key not set after 5F48-absent rejection");
}

/* 7F48 declares 92 as 32 bytes but 5F48 only carries 16 bytes → 6A80. */
static void test_key_import_5f48_too_short(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d16[16] = {0x88};
    uint8_t inner[64]; uint16_t ii = 0;
    inner[ii++]=0xB6; inner[ii++]=0x00;
    inner[ii++]=0x7F; inner[ii++]=0x48; inner[ii++]=0x02;
    inner[ii++]=0x92; inner[ii++]=0x20; /* template declares 32 B */
    inner[ii++]=0x5F; inner[ii++]=0x48; inner[ii++]=0x10; /* 5F48 only 16 B */
    memcpy(inner + ii, d16, 16); ii += 16;

    cmd[0]=0x00; cmd[1]=0xDB; cmd[2]=0x3F; cmd[3]=0xFF;
    cmd[4]=(uint8_t)(2u + ii); cmd[5]=0x4D; cmd[6]=(uint8_t)ii;
    memcpy(cmd + 7, inner, ii);
    uint16_t clen = (uint16_t)(7u + ii);

    uint16_t rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80,
                   "key import with 5F48 too short (16 < 32) returns 6A80");
    TEST_ASSERT(!openpgp_card_key_is_set(), "key not set after 5F48-too-short rejection");
}

/* ------------------------------------------------------------------ */
/* Tests — Task 6: CHANGE REFERENCE DATA + PIN persistence            */
/* ------------------------------------------------------------------ */

/* Happy-path PW1 change: old "123456" → new "654321"; old PIN fails after. */
static void test_change_pw1(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    static const uint8_t old_pw1[] = {'1','2','3','4','5','6'};
    static const uint8_t new_pw1[] = {'6','5','4','3','2','1'};

    clen = build_change_ref(0x81, old_pw1, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "CHANGE PW1 returns 9000");

    /* Old PIN must now fail */
    clen = build_verify(0x81, old_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "old PW1 rejected after change (63C2)");

    /* New PIN must succeed */
    clen = build_verify(0x81, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "new PW1 accepted after change (9000)");
}

/* Happy-path PW3 change: 8-byte default → 8-byte new value. */
static void test_change_pw3(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    static const uint8_t old_pw3[] = {'1','2','3','4','5','6','7','8'};
    static const uint8_t new_pw3[] = {'8','7','6','5','4','3','2','1'};

    clen = build_change_ref(0x83, old_pw3, 8, new_pw3, 8, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "CHANGE PW3 returns 9000");

    /* Old PIN must now fail */
    clen = build_verify(0x83, old_pw3, 8, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "old PW3 rejected after change (63C2)");

    /* New PIN must succeed */
    clen = build_verify(0x83, new_pw3, 8, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "new PW3 accepted after change (9000)");
}

/* Error-path rules: wrong old → 63Cx; new too short → 6A80 (no decrement);
 * P2=0x82 → 6A86; blocked counter → 6983. */
static void test_change_pw_rules(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    static const uint8_t old_pw1[]   = {'1','2','3','4','5','6'};
    static const uint8_t new_pw1[]   = {'6','5','4','3','2','1'};
    static const uint8_t wrong_old[] = {'X','X','X','X','X','X'};

    /* 1. Wrong old PIN → retry decrement → 63C2 */
    clen = build_change_ref(0x81, wrong_old, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "CHANGE wrong old -> 63C2");

    /* 2. New PIN too short (4 < min 6) → 6A80, counter must NOT decrement */
    static const uint8_t short_new[] = {'1','2','3','4'};
    clen = build_change_ref(0x81, old_pw1, 6, short_new, 4, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80, "CHANGE new too short -> 6A80");
    /* Verify counter is still 2 (no decrement from the 6A80 path) */
    {
        uint8_t chk[5] = {0x00, 0x20, 0x00, 0x81, 0x00};
        rlen = openpgp_card_apdu(chk, 5, rsp, sizeof(rsp));
        TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2,
                       "counter still 2 after 6A80 (no decrement)");
    }

    /* 3. P2=0x82 → 6A86 (not a valid CHANGE REF DATA P2) */
    clen = build_change_ref(0x82, old_pw1, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A86, "CHANGE P2=82 -> 6A86");

    /* 4. Block the counter then attempt → 6983 */
    /* Counter is at 2; exhaust remaining 2 retries */
    clen = build_change_ref(0x81, wrong_old, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C1, "CHANGE 2nd wrong -> 63C1");
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C0, "CHANGE 3rd wrong -> 63C0 (blocked)");
    /* Correct old now blocked */
    clen = build_change_ref(0x81, old_pw1, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6983, "CHANGE on blocked PW1 -> 6983");
}

/* Successful CHANGE 81 must clear s_pw1_sign_verified → PSO:CDS fails with 6982. */
static void test_change_pw_clears_verified(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0xCC};
    do_import_key(d, cmd, rsp);
    do_disable_uif(cmd, rsp);

    /* Establish signing authorisation */
    do_verify_pw1_sign(cmd, rsp);

    /* Change PW1 — must clear the verified flag */
    static const uint8_t old_pw1[] = {'1','2','3','4','5','6'};
    static const uint8_t new_pw1[] = {'6','5','4','3','2','1'};
    clen = build_change_ref(0x81, old_pw1, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "CHANGE PW1 returns 9000");

    /* PSO:CDS must now fail — flag cleared by CHANGE */
    uint8_t hash[32]; memset(hash, 0xEE, 32);
    clen = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982,
                   "PSO:CDS after CHANGE PW1 returns 6982 (flag cleared)");
}

/* Verify that pin_persist() is called exactly when specified. */
static void test_pin_persist_triggers(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);
    /* Reset counter AFTER setup_card (factory_reset already ticked it) */
    g_pin_persist_calls = 0;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* Wrong VERIFY → retry decrement → persist */
    static const uint8_t wrong[] = {'X','X','X','X','X','X'};
    static const uint8_t pw1[]   = {'1','2','3','4','5','6'};
    clen = build_verify(0x81, wrong, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x63C2, "wrong verify -> 63C2");
    TEST_ASSERT_EQ(g_pin_persist_calls, 1, "persist==1 after wrong verify");

    /* Correct VERIFY (counter was 2, below max 3) → persist */
    clen = build_verify(0x81, pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "correct verify -> 9000");
    TEST_ASSERT_EQ(g_pin_persist_calls, 2, "persist==2 after correct verify (counter restored)");

    /* Correct VERIFY again (counter already at max 3) → NO persist */
    clen = build_verify(0x81, pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "second correct verify -> 9000");
    TEST_ASSERT_EQ(g_pin_persist_calls, 2, "persist still==2 (counter was already max)");

    /* CHANGE PW1 with correct old → persist */
    static const uint8_t new_pw1[] = {'6','5','4','3','2','1'};
    clen = build_change_ref(0x81, pw1, 6, new_pw1, 6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000, "change pw1 -> 9000");
    TEST_ASSERT_EQ(g_pin_persist_calls, 3, "persist==3 after CHANGE PW1");
}

/* ------------------------------------------------------------------ */
/* APDU builder — INS 0x47 (GENERATE ASYMMETRIC KEY PAIR)             */
/* ------------------------------------------------------------------ */

/* Build: 00 47 <p1> 00 02 <crt_tag> 00  (CRT with empty value) */
static uint16_t build_read_pubkey(uint8_t p1, uint8_t crt_tag, uint8_t *buf)
{
    buf[0] = 0x00; buf[1] = 0x47; buf[2] = p1; buf[3] = 0x00;
    buf[4] = 0x02;    /* Lc */
    buf[5] = crt_tag;
    buf[6] = 0x00;    /* empty value */
    return 7;
}

/* ------------------------------------------------------------------ */
/* Tests — INS 0x47 P1=0x81 READ PUBLIC KEY                           */
/* ------------------------------------------------------------------ */

/* No key imported → 6A88 */
static void test_read_pubkey_no_key(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_read_pubkey(0x81, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88,
                   "READ PUBKEY (B6) without imported key → 6A88");
}

/* Key imported → 9000, response = 7F49 43 86 41 04 ... (70 bytes data),
   pubkey bytes match fake_pubkey output for the imported scalar. */
static void test_read_pubkey_ok(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    uint8_t d[32];
    for (int i = 0; i < 32; i++) d[i] = (uint8_t)(0xA0 + i);
    do_import_key(d, cmd, rsp);

    clen = build_read_pubkey(0x81, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "READ PUBKEY (B6) with imported key → 9000");

    uint16_t dlen = (uint16_t)(rlen - 2);
    TEST_ASSERT_EQ(dlen, 70, "READ PUBKEY response data is 70 bytes");

    /* Verify 7F 49 43 86 41 04 framing */
    TEST_ASSERT_EQ(rsp[0], 0x7F, "response[0] = 7F (outer tag hi)");
    TEST_ASSERT_EQ(rsp[1], 0x49, "response[1] = 49 (outer tag lo)");
    TEST_ASSERT_EQ(rsp[2], 0x43, "response[2] = 43 (inner len = 67)");
    TEST_ASSERT_EQ(rsp[3], 0x86, "response[3] = 86 (public key tag)");
    TEST_ASSERT_EQ(rsp[4], 0x41, "response[4] = 41 (public point len = 65)");
    TEST_ASSERT_EQ(rsp[5], 0x04, "response[5] = 04 (uncompressed point)");

    /* fake_pubkey output: 0x04 || d || d */
    uint8_t expected[65];
    expected[0] = 0x04;
    memcpy(expected + 1,  d, 32);
    memcpy(expected + 33, d, 32);
    TEST_ASSERT(memcmp(rsp + 5, expected, 65) == 0,
                "READ PUBKEY pubkey bytes match fake_pubkey(d) output");
}

/* P1=0x80 (on-device GENERATE) → 6D00 — not implemented in Phase 1. */
static void test_read_pubkey_generate_unsupported(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[16], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    clen = build_read_pubkey(0x80, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6D00,
                   "GENERATE PUBKEY (P1=0x80) → 6D00 (unsupported)");
}

/* Auth slot (A4) — only sig slot (B6) supported in Phase 1 → 6A88. */
static void test_read_pubkey_wrong_slot(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0xDD};
    do_import_key(d, cmd, rsp);

    /* A4 = auth slot → 6A88 even though a sig key is present */
    clen = build_read_pubkey(0x81, 0xA4, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88,
                   "READ PUBKEY for auth slot (A4) → 6A88");
}

/* .pubkey hook NULL → 6A88 even when key is present.
 * This test would have caught the original wiring bug in ccid.c where
 * s_dongle_hooks.pubkey was left NULL, making INS 0x47 always dead. */
static void test_read_pubkey_null_hook(void)
{
    /* Use a hooks struct where .pubkey is explicitly NULL */
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm, .pubkey = NULL };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);

    /* Import a real key so the 6A88 comes from the NULL hook, not missing key */
    do_verify_pw3(cmd, rsp);
    static const uint8_t d[32] = {0xEE};
    do_import_key(d, cmd, rsp);

    /* READ PUBLIC KEY (00 47 81 00 02 B6 00) → 6A88 because .pubkey is NULL */
    clen = build_read_pubkey(0x81, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A88,
                   "READ PUBKEY with NULL .pubkey hook → 6A88");
}

/* ------------------------------------------------------------------ */
/* Tests — security hardening fixes                                    */
/* ------------------------------------------------------------------ */

/* Fix 1: crypto health gate.
 * When s_crypto_ok is false, PSO:CDS must return SW 0x6581 (memory failure)
 * before any key or PIN check.  GET DATA 6E must remain reachable (9000) so
 * the host can still read diagnostic card status. */
static void test_crypto_broken_blocks_sign(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm,
                                .pubkey = fake_pubkey };
    setup_card(&h);  /* sets health true */
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0x11};
    do_import_key(d, cmd, rsp);
    do_disable_uif(cmd, rsp);

    /* Establish a valid PW1 signing authorisation before tripping health */
    do_verify_pw1_sign(cmd, rsp);

    /* Trip the health flag — simulates a failed KAT at boot */
    openpgp_card_set_crypto_health(false);

    uint8_t hash[32]; memset(hash, 0xCD, 32);
    clen = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6581,
                   "PSO:CDS with broken crypto health returns 6581");

    /* GET DATA 6E (diagnostic) must still work — crypto gate must not
     * affect GET DATA, SELECT, or VERIFY. */
    clen = build_get_data(0x00, 0x6E, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x9000,
                   "GET DATA 6E still returns 9000 when crypto health is false");

    /* Restore for subsequent tests */
    openpgp_card_set_crypto_health(true);
}

/* Fix 1: crypto health gate for READ PUBLIC KEY (INS 0x47 P1=0x81). */
static void test_crypto_broken_blocks_readpub(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm,
                                .pubkey = fake_pubkey };
    setup_card(&h);  /* sets health true */

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0x22};
    do_import_key(d, cmd, rsp);

    openpgp_card_set_crypto_health(false);

    clen = build_read_pubkey(0x81, 0xB6, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6581,
                   "READ PUBLIC KEY with broken crypto health returns 6581");

    /* Restore */
    openpgp_card_set_crypto_health(true);
}

/* Fix 2: PW1 is consumed BEFORE sign() is called, so a failing sign still
 * burns the single-use token.  A second PSO:CDS without a new VERIFY 81
 * must return 6982 (security status not satisfied). */
static void test_sign_consumes_pw1_on_failure(void)
{
    /* Use the always-failing sign hook */
    openpgp_card_hooks_t h = { .sign = fake_sign_fail, .confirm = fake_confirm,
                                .pubkey = fake_pubkey };
    setup_card(&h);
    g_confirm_retval = 1;

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    static const uint8_t d[32] = {0x55};
    do_import_key(d, cmd, rsp);
    do_disable_uif(cmd, rsp);  /* keep UIF out of the equation */

    do_verify_pw1_sign(cmd, rsp);

    uint8_t hash[32]; memset(hash, 0xAA, 32);

    /* First attempt: sign hook fails → SW_COND_NOT_SAT (6985).
     * PW1 must have been consumed already. */
    clen = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6985,
                   "PSO:CDS with failing sign hook returns 6985");

    /* Second attempt without re-VERIFY: PW1 was consumed → 6982. */
    clen = build_pso_cds(hash, 32, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6982,
                   "second PSO:CDS without re-VERIFY after sign failure returns 6982");
}

/* Fix 2: importing a private scalar of all-zero bytes must be rejected
 * with SW 6A80 and the key must not be set in the applet state. */
static void test_import_rejects_zero_scalar(void)
{
    openpgp_card_hooks_t h = { .sign = fake_sign, .confirm = fake_confirm,
                                .pubkey = fake_pubkey };
    setup_card(&h);

    uint8_t cmd[64], rsp[256];
    uint16_t clen, rlen;

    do_select(cmd, rsp);
    do_verify_pw3(cmd, rsp);

    /* All-zero scalar — invalid/exploitable private key material */
    static const uint8_t zero_d[32] = {0};
    clen = build_key_import(zero_d, cmd);
    rlen = openpgp_card_apdu(cmd, clen, rsp, sizeof(rsp));
    TEST_ASSERT_EQ(sw_of(rsp, rlen), 0x6A80,
                   "key import with all-zero scalar returns 6A80");
    TEST_ASSERT(!openpgp_card_key_is_set(),
                "key not set after zero-scalar rejection");
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                              */
/* ------------------------------------------------------------------ */

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
    /* HW-validated fix: 5E login data + 7F74 general feature management */
    TEST_RUN(test_login_data_empty);
    TEST_RUN(test_gf_management);
    TEST_RUN(test_6e_contains_7f74);
    /* Task 5: key import (PUT DATA 0xDB 3FFF) + DS counter + keyed sign */
    TEST_RUN(test_key_import_requires_pw3);
    TEST_RUN(test_key_import_ok_and_sign_uses_it);
    TEST_RUN(test_key_import_b6_variant);
    TEST_RUN(test_key_import_wrong_slot_b8);
    TEST_RUN(test_key_import_bad_92_len);
    TEST_RUN(test_sign_without_key_6a88);
    TEST_RUN(test_sign_consumes_pw1);
    TEST_RUN(test_ds_counter_increments);
    /* W4: negative import paths — 7F48/5F48 absent, 5F48 too short */
    TEST_RUN(test_key_import_7f48_absent);
    TEST_RUN(test_key_import_5f48_absent);
    TEST_RUN(test_key_import_5f48_too_short);
    /* Task 6: CHANGE REFERENCE DATA + PIN persistence */
    TEST_RUN(test_change_pw1);
    TEST_RUN(test_change_pw3);
    TEST_RUN(test_change_pw_rules);
    TEST_RUN(test_change_pw_clears_verified);
    TEST_RUN(test_pin_persist_triggers);
    /* Task 8b: INS 0x47 P1=0x81 READ PUBLIC KEY */
    TEST_RUN(test_read_pubkey_no_key);
    TEST_RUN(test_read_pubkey_ok);
    TEST_RUN(test_read_pubkey_generate_unsupported);
    TEST_RUN(test_read_pubkey_wrong_slot);
    TEST_RUN(test_read_pubkey_null_hook);
    /* Security hardening: crypto health gate, consume PW1 before sign, zero scalar, const-time PIN */
    TEST_RUN(test_crypto_broken_blocks_sign);
    TEST_RUN(test_crypto_broken_blocks_readpub);
    TEST_RUN(test_sign_consumes_pw1_on_failure);
    TEST_RUN(test_import_rejects_zero_scalar);
}
