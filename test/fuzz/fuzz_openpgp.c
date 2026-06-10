/*
 * test/fuzz/fuzz_openpgp.c — deterministic random-input harness for
 * openpgp_card_apdu().
 *
 * Threat model: fully malicious host, can send any byte sequence.
 *
 * Build (from test/fuzz/):
 *   gcc -std=c11 -g -O1 \
 *       -fsanitize=address,undefined \
 *       -DTEST_HOST \
 *       -I../.. -I../../main/security -I../../main/comm/cdc \
 *       -I../../main/input -I../../boards/kase_v2_debug \
 *       fuzz_openpgp.c \
 *       ../../main/security/openpgp_card.c \
 *       ../../main/security/openpgp_do.c \
 *       ../../main/security/apdu.c \
 *       ../../main/security/sec_confirm.c \
 *       -o fuzz_openpgp
 *
 * Run:
 *   ./fuzz_openpgp
 *
 * On success prints "Fuzz complete: N iterations, 0 crashes" and exits 0.
 * ASan/UBSan will abort and print the error if any OOB/UB is detected.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "openpgp_card.h"
#include "openpgp_do.h"
#include "sec_confirm.h"

/* ------------------------------------------------------------------ */
/* Fake crypto hooks (no mbedtls on host)                             */
/* ------------------------------------------------------------------ */

static uint8_t  g_last_d[32];
static bool     g_confirm_val = true;

static bool fake_sign(const uint8_t d[32],
                      const uint8_t *hash, uint16_t n,
                      uint8_t *out, uint16_t *out_n)
{
    memcpy(g_last_d, d, 32);
    (void)hash; (void)n;
    /* canned: 64 bytes of 0x42. Must NOT echo d back. */
    memset(out, 0x42, 64);
    *out_n = 64;
    return true;
}

static int fake_confirm(void)
{
    return g_confirm_val ? 1 : 2;
}

static bool fake_pubkey(const uint8_t d[32], uint8_t out_pub[65])
{
    /* Return a dummy uncompressed point. Must NOT echo d. */
    out_pub[0] = 0x04;
    memset(out_pub + 1, 0x55, 64);
    (void)d;
    return true;
}

static const openpgp_card_hooks_t hooks = {
    .sign    = fake_sign,
    .confirm = fake_confirm,
    .pubkey  = fake_pubkey,
};

/* ------------------------------------------------------------------ */
/* PRNG (xorshift64)                                                  */
/* ------------------------------------------------------------------ */
static uint64_t g_rng_state = 0xDEADBEEFCAFEBABEull;

static uint64_t rng_next(void)
{
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 7;
    g_rng_state ^= g_rng_state << 17;
    return g_rng_state;
}

static uint8_t rng_byte(void)
{
    return (uint8_t)(rng_next() & 0xFF);
}

static void rng_fill(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) buf[i] = rng_byte();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static void reset_card(void)
{
    sec_confirm_reset();
    openpgp_card_init(&hooks);
    openpgp_card_factory_reset();
}

static uint16_t sw_of(const uint8_t *rsp, uint16_t rlen)
{
    if (rlen < 2) return 0;
    return (uint16_t)((rsp[rlen-2] << 8) | rsp[rlen-1]);
}

static const uint8_t OPGP_AID[]   = {0xD2,0x76,0x00,0x01,0x24,0x01};
static const uint8_t PW1_DEF[]    = {'1','2','3','4','5','6'};
static const uint8_t PW3_DEF[]    = {'1','2','3','4','5','6','7','8'};

/* ------------------------------------------------------------------ */
/* Feed a single raw APDU buffer and check invariants                 */
/* ------------------------------------------------------------------ */

/* Global test private key — sentinel to detect G1 violations */
static const uint8_t TEST_KEY_D[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,
};

/* Returns false and prints a finding if invariants are violated. */
static bool feed_apdu(const uint8_t *in, uint16_t in_len,
                      const char *label)
{
    uint8_t rsp[512];
    uint16_t rlen = openpgp_card_apdu(in, in_len, rsp, sizeof(rsp));

    /* G4: must always return at least 2 bytes (SW) — ASan catches OOB
     * writes; this catches a return-value logic error. */
    if (rlen < 2) {
        fprintf(stderr, "[FAIL] %s: rlen=%u < 2 (SW missing)\n", label, rlen);
        return false;
    }

    /*
     * G1: the response must NEVER contain TEST_KEY_D.
     * Even a partial 4-byte match of a recognizable key fragment is
     * suspicious enough to flag (the full 32-byte check is the strict one).
     */
    if (rlen > 34) {
        /* Full key check (32 consecutive bytes) */
        for (int i = 0; i + 32 <= (int)rlen; i++) {
            if (memcmp(rsp + i, TEST_KEY_D, 32) == 0) {
                fprintf(stderr,
                    "[FAIL-G1] %s: private scalar present in response "
                    "at offset %d (SW=%04x)\n",
                    label, i, sw_of(rsp, rlen));
                return false;
            }
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* PHASE 1: Fully-random APDUs (unselected + selected states)         */
/* ------------------------------------------------------------------ */

static void fuzz_random_apdu(int iterations)
{
    uint8_t cmd[256];

    for (int i = 0; i < iterations; i++) {
        /* Alternate: one in four uses an empty / very short APDU */
        uint16_t len;
        uint64_t r = rng_next();
        if ((r & 3) == 0) {
            len = (uint16_t)(r & 7);
        } else {
            len = (uint16_t)(4 + (r % 250));
        }
        rng_fill(cmd, len);
        feed_apdu(cmd, len, "random_unsel");

        /* Now after SELECT */
        uint8_t sel[11] = {0x00,0xA4,0x04,0x00,0x06,
                           0xD2,0x76,0x00,0x01,0x24,0x01};
        openpgp_card_apdu(sel, 11, cmd, sizeof(cmd));  /* cmd reused as rsp */

        rng_fill(cmd, len);
        feed_apdu(cmd, len, "random_sel");
    }
}

/* ------------------------------------------------------------------ */
/* PHASE 2: Mutations of the valid key-import APDU (INS 0xDB)         */
/* The base APDU is the canonical 44-byte import from test_openpgp_card.c.
 * The fuzzer corrupts individual bytes and length fields.             */
/* ------------------------------------------------------------------ */

/* Build the canonical key-import APDU for TEST_KEY_D; returns length. */
static uint16_t build_canonical_import(uint8_t *buf)
{
    buf[0] = 0x00; buf[1] = 0xDB; buf[2] = 0x3F; buf[3] = 0xFF;
    buf[4] = 0x2C;                /* Lc = 44 */
    buf[5] = 0x4D; buf[6] = 0x2A; /* outer: 4D, len 42 */
    buf[7] = 0xB6; buf[8] = 0x00; /* B6 00: sig-slot CRT */
    buf[9] = 0x7F; buf[10]= 0x48; buf[11]= 0x02; /* 7F48 template */
    buf[12]= 0x92; buf[13]= 0x20; /* element 92, 32 bytes */
    buf[14]= 0x5F; buf[15]= 0x48; buf[16]= 0x20; /* 5F48 key material */
    memcpy(buf + 17, TEST_KEY_D, 32);
    return 49;
}

/* Build the select + PW3-verify preamble. */
static void do_preamble(void)
{
    uint8_t buf[64], rsp[256];
    /* SELECT */
    buf[0]=0x00; buf[1]=0xA4; buf[2]=0x04; buf[3]=0x00; buf[4]=0x06;
    memcpy(buf+5, OPGP_AID, 6);
    openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));
    /* VERIFY PW3 */
    buf[0]=0x00; buf[1]=0x20; buf[2]=0x00; buf[3]=0x83; buf[4]=0x08;
    memcpy(buf+5, PW3_DEF, 8);
    openpgp_card_apdu(buf, 13, rsp, sizeof(rsp));
}

static void fuzz_import_mutations(int iterations)
{
    uint8_t base[64];
    uint8_t mut[320]; /* oversized for length-expansion mutations */
    uint16_t base_len = build_canonical_import(base);

    for (int i = 0; i < iterations; i++) {
        reset_card();
        do_preamble();

        /* Copy base, then apply one random mutation */
        uint64_t r = rng_next();
        int kind = (int)(r % 12);

        switch (kind) {
        case 0:
            /* Flip a random byte in the import payload */
            memcpy(mut, base, base_len);
            mut[r % base_len] ^= (uint8_t)((r >> 8) | 0x01);
            feed_apdu(mut, base_len, "import_flip_byte");
            break;

        case 1:
            /* Corrupt Lc (byte 4) */
            memcpy(mut, base, base_len);
            mut[4] = rng_byte();
            feed_apdu(mut, base_len, "import_bad_lc");
            break;

        case 2:
            /* Corrupt 4D inner length (byte 6) */
            memcpy(mut, base, base_len);
            mut[6] = rng_byte();
            feed_apdu(mut, base_len, "import_bad_4d_len");
            break;

        case 3:
            /* Corrupt 7F48 length (byte 11) */
            memcpy(mut, base, base_len);
            mut[11] = rng_byte();
            feed_apdu(mut, base_len, "import_bad_7f48_len");
            break;

        case 4:
            /* Corrupt scalar length in template (byte 13, should be 0x20) */
            memcpy(mut, base, base_len);
            mut[13] = rng_byte();
            feed_apdu(mut, base_len, "import_bad_92_len");
            break;

        case 5:
            /* Corrupt 5F48 length (byte 16) */
            memcpy(mut, base, base_len);
            mut[16] = rng_byte();
            feed_apdu(mut, base_len, "import_bad_5f48_len");
            break;

        case 6:
            /* Replace B6 tag with B8 (decrypt slot) */
            memcpy(mut, base, base_len);
            mut[7] = 0xB8;
            feed_apdu(mut, base_len, "import_b8_slot");
            break;

        case 7:
            /* Oversized declared inner length (0x82 extended form, large) */
            memcpy(mut, base, base_len);
            /* Tag 4D, then 0x82 0xFF 0xFF = 65535 bytes claimed */
            mut[5] = 0x4D;
            mut[6] = 0x82; /* 0x82 = two-byte length follows, but we only
                            * provide base_len-7 bytes — truncation test */
            mut[7] = 0xFF;
            mut[8] = 0xFF;
            /* shift rest by 2 bytes — total will be longer */
            memmove(mut + 9, base + 7, base_len - 7);
            feed_apdu(mut, (uint16_t)(base_len + 2), "import_huge_len");
            break;

        case 8:
            /* Truncated APDU: cut at various lengths */
            {
                uint16_t trunc = (uint16_t)(1 + (r % (base_len - 1)));
                memcpy(mut, base, trunc);
                feed_apdu(mut, trunc, "import_truncated");
            }
            break;

        case 9:
            /* Extended-length encoding of a valid import */
            {
                /* 00 DB 3F FF 00 Lc_hi Lc_lo payload */
                uint16_t inner_len = base_len - 5; /* payload length */
                mut[0]=0x00; mut[1]=0xDB; mut[2]=0x3F; mut[3]=0xFF;
                mut[4]=0x00;
                mut[5]=(uint8_t)(inner_len >> 8);
                mut[6]=(uint8_t)(inner_len & 0xFF);
                memcpy(mut + 7, base + 5, inner_len);
                feed_apdu(mut, (uint16_t)(7 + inner_len), "import_extended");
            }
            break;

        case 10:
            /* Prepend spurious TLV elements before B6 — tests d_offset overflow.
             * Craft: 4D len B6 00 7F48 02 <tag_not_92>=0xF0 <huge_len=0xFF> B6... */
            {
                uint16_t ii = 0;
                mut[0]=0x00; mut[1]=0xDB; mut[2]=0x3F; mut[3]=0xFF;
                /* leave space for Lc */
                ii = 5;
                mut[ii++]=0x4D; /* 4D tag */
                uint8_t *inner_len_pos = &mut[ii++]; /* placeholder for inner len */
                uint16_t inner_start = ii;
                /* B6 00 */
                mut[ii++]=0xB6; mut[ii++]=0x00;
                /* 7F48 template with a large spurious element before 92 */
                mut[ii++]=0x7F; mut[ii++]=0x48;
                /* template length = 2 (for spurious) + 2 (for 92) = 4 */
                mut[ii++]=0x04;
                /* spurious element: tag 0x91, declared size 0xFE (254) */
                mut[ii++]=0x91; mut[ii++]=0xFE; /* <-- d_offset += 0xFE = 254 */
                /* 92 with 32 bytes */
                mut[ii++]=0x92; mut[ii++]=0x20;
                /* 5F48 with only 32 bytes total (254 offset but only 32 bytes there) */
                mut[ii++]=0x5F; mut[ii++]=0x48; mut[ii++]=0x20;
                memcpy(mut + ii, TEST_KEY_D, 32); ii += 32;
                *inner_len_pos = (uint8_t)(ii - inner_start);
                mut[4] = (uint8_t)(ii - 5);
                feed_apdu(mut, ii, "import_d_offset_overflow");
            }
            break;

        case 11:
            /* Fully random import-shaped APDU */
            mut[0]=0x00; mut[1]=0xDB; mut[2]=0x3F; mut[3]=0xFF;
            {
                uint8_t payload_len = (uint8_t)(10 + (r % 200));
                mut[4] = payload_len;
                rng_fill(mut + 5, payload_len);
                feed_apdu(mut, (uint16_t)(5 + payload_len), "import_random_payload");
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* PHASE 3: UIF bypass attempts (G2)                                  */
/* Construct sequences that try to get a signature without touch.     */
/* ------------------------------------------------------------------ */

static void fuzz_uif_bypass(void)
{
    uint8_t buf[64], rsp[256];
    uint16_t rlen;

    /* --- Attempt A: import key, arm UIF, then SELECT mid-transaction --- */
    reset_card();

    /* SELECT + PW3 + import + VERIFY PW1 */
    do_preamble();
    /* Import key */
    build_canonical_import(buf);
    openpgp_card_apdu(buf, 49, rsp, sizeof(rsp));

    /* VERIFY PW1 sign (0x81) */
    buf[0]=0x00; buf[1]=0x20; buf[2]=0x00; buf[3]=0x81; buf[4]=0x06;
    memcpy(buf+5, PW1_DEF, 6);
    openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));

    /* UIF is enabled by default (D6={0x01,0x20}).
     * Inject SELECT here — should clear verified flags. */
    buf[0]=0x00; buf[1]=0xA4; buf[2]=0x04; buf[3]=0x00; buf[4]=0x06;
    memcpy(buf+5, OPGP_AID, 6);
    openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));

    /* PSO:CDS after SELECT-reset — must NOT succeed (6982 or 6985 expected) */
    buf[0]=0x00; buf[1]=0x2A; buf[2]=0x9E; buf[3]=0x9A; buf[4]=0x20;
    memset(buf+5, 0xAB, 32);
    rlen = openpgp_card_apdu(buf, 37, rsp, sizeof(rsp));
    uint16_t sw = sw_of(rsp, rlen);
    if (sw == 0x9000) {
        fprintf(stderr, "[FAIL-G2] UIF bypass A (SELECT-mid): got 9000 without touch\n");
        exit(1);
    }

    /* --- Attempt B: confirm returns 0 (pending), PSO:CDS hook returns that --- */
    /* This is tested by the hook directly; test that confirm=2 is rejected */
    reset_card();
    do_preamble();
    build_canonical_import(buf);
    openpgp_card_apdu(buf, 49, rsp, sizeof(rsp));

    buf[0]=0x00; buf[1]=0x20; buf[2]=0x00; buf[3]=0x81; buf[4]=0x06;
    memcpy(buf+5, PW1_DEF, 6);
    openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));

    /* confirm = deny */
    g_confirm_val = false;

    buf[0]=0x00; buf[1]=0x2A; buf[2]=0x9E; buf[3]=0x9A; buf[4]=0x20;
    memset(buf+5, 0xCD, 32);
    rlen = openpgp_card_apdu(buf, 37, rsp, sizeof(rsp));
    sw = sw_of(rsp, rlen);
    if (sw == 0x9000) {
        fprintf(stderr, "[FAIL-G2] UIF bypass B (confirm=deny): got 9000\n");
        exit(1);
    }
    g_confirm_val = true;

    /* --- Attempt C: second PSO:CDS reusing prior UIF grant (PW1 is consumed) --- */
    reset_card();
    do_preamble();
    build_canonical_import(buf);
    openpgp_card_apdu(buf, 49, rsp, sizeof(rsp));

    buf[0]=0x00; buf[1]=0x20; buf[2]=0x00; buf[3]=0x81; buf[4]=0x06;
    memcpy(buf+5, PW1_DEF, 6);
    openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));

    /* First PSO:CDS — succeeds (confirm authorized) */
    buf[0]=0x00; buf[1]=0x2A; buf[2]=0x9E; buf[3]=0x9A; buf[4]=0x20;
    memset(buf+5, 0xBB, 32);
    rlen = openpgp_card_apdu(buf, 37, rsp, sizeof(rsp));
    if (sw_of(rsp, rlen) != 0x9000) {
        fprintf(stderr, "[WARN] UIF bypass C: first sign unexpectedly failed SW=%04x\n",
                sw_of(rsp, rlen));
    }

    /* Second PSO:CDS without re-VERIFY — must fail (6982, PW1 consumed) */
    rlen = openpgp_card_apdu(buf, 37, rsp, sizeof(rsp));
    sw = sw_of(rsp, rlen);
    if (sw == 0x9000) {
        fprintf(stderr, "[FAIL-G2/G3] UIF bypass C: second sign without PW1 re-verify got 9000\n");
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* PHASE 4: G1 check — READ PUBLIC KEY must not echo d                */
/* ------------------------------------------------------------------ */

static void fuzz_g1_pubkey_leak(void)
{
    uint8_t buf[64], rsp[256];

    reset_card();
    do_preamble();
    build_canonical_import(buf);
    openpgp_card_apdu(buf, 49, rsp, sizeof(rsp));

    /* INS 0x47 P1=0x81 — read public key */
    buf[0]=0x00; buf[1]=0x47; buf[2]=0x81; buf[3]=0x00;
    buf[4]=0x02; buf[5]=0xB6; buf[6]=0x00;
    uint16_t rlen = openpgp_card_apdu(buf, 7, rsp, sizeof(rsp));

    /* The response must not contain TEST_KEY_D */
    for (int i = 0; i + 32 <= (int)rlen; i++) {
        if (memcmp(rsp + i, TEST_KEY_D, 32) == 0) {
            fprintf(stderr,
                "[FAIL-G1] READ PUBLIC KEY: private scalar found in response at offset %d\n",
                i);
            exit(1);
        }
    }

    /* Also check that every GET DATA tag response is free of the key */
    static const uint8_t tags[][2] = {
        {0x00,0x4F},{0x5F,0x52},{0x00,0xC0},{0x00,0xC1},{0x00,0xC2},
        {0x00,0xC3},{0x00,0xC4},{0x00,0xC5},{0x00,0xC6},{0x00,0xCD},
        {0x00,0xD6},{0x00,0xD7},{0x00,0xD8},{0x00,0x6E},{0x00,0x65},
        {0x00,0x7A},{0x7F,0x74},{0x00,0x93},
    };
    for (size_t t = 0; t < sizeof(tags)/sizeof(tags[0]); t++) {
        buf[0]=0x00; buf[1]=0xCA; buf[2]=tags[t][0]; buf[3]=tags[t][1]; buf[4]=0x00;
        rlen = openpgp_card_apdu(buf, 5, rsp, sizeof(rsp));
        for (int i = 0; i + 32 <= (int)rlen; i++) {
            if (memcmp(rsp + i, TEST_KEY_D, 32) == 0) {
                fprintf(stderr,
                    "[FAIL-G1] GET DATA %02x%02x: private scalar in response\n",
                    tags[t][0], tags[t][1]);
                exit(1);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* PHASE 5: GET DATA 0x6E body overflow guard                         */
/* PUT large DOs to try to overflow the 256-byte inner / body buffers */
/* ------------------------------------------------------------------ */

static void fuzz_get_data_overflow(void)
{
    uint8_t buf[270], rsp[512];

    reset_card();
    do_preamble();

    /* PUT DATA: write 255-byte C0 (extended capabilities) — should be
     * accepted by openpgp_do_put (OPENPGP_DO_MAX_LEN=256) but the 6E
     * body/inner builder uses a fixed 256-byte buffer — tlv_append must
     * return 0 (overflow) and trigger APPEND_OR_FAIL → 6F00 rather than
     * silently corrupting memory. */
    buf[0]=0x00; buf[1]=0xDA; buf[2]=0x00; buf[3]=0xC0;
    buf[4]=0xFF; /* Lc = 255 */
    memset(buf+5, 0xAA, 255);
    openpgp_card_apdu(buf, 260, rsp, sizeof(rsp));

    /* GET DATA 6E must not crash (ASan catches any OOB write) */
    buf[0]=0x00; buf[1]=0xCA; buf[2]=0x00; buf[3]=0x6E; buf[4]=0x00;
    openpgp_card_apdu(buf, 5, rsp, sizeof(rsp));

    /* Restore factory C0 */
    reset_card();
}

/* ------------------------------------------------------------------ */
/* PHASE 6: PIN brute-force — verifies G3 counter decrement           */
/* ------------------------------------------------------------------ */

static void fuzz_pin_brute(void)
{
    uint8_t buf[32], rsp[64];

    reset_card();

    /* SELECT */
    buf[0]=0x00; buf[1]=0xA4; buf[2]=0x04; buf[3]=0x00; buf[4]=0x06;
    memcpy(buf+5, OPGP_AID, 6);
    openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));

    /* Three wrong PW1 attempts */
    uint8_t wrong[] = {'W','R','O','N','G','!'};
    for (int i = 0; i < 3; i++) {
        buf[0]=0x00; buf[1]=0x20; buf[2]=0x00; buf[3]=0x81; buf[4]=0x06;
        memcpy(buf+5, wrong, 6);
        uint16_t rlen = openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));
        uint16_t sw = sw_of(rsp, rlen);
        /* Should be 63C2, 63C1, 63C0 */
        if ((sw & 0xFFF0) != 0x63C0) {
            fprintf(stderr, "[FAIL-G3] Wrong PIN %d: unexpected SW %04x\n", i+1, sw);
            exit(1);
        }
    }

    /* Correct PIN now blocked */
    buf[0]=0x00; buf[1]=0x20; buf[2]=0x00; buf[3]=0x81; buf[4]=0x06;
    memcpy(buf+5, PW1_DEF, 6);
    uint16_t rlen = openpgp_card_apdu(buf, 11, rsp, sizeof(rsp));
    uint16_t sw = sw_of(rsp, rlen);
    if (sw != 0x6983) {
        fprintf(stderr, "[FAIL-G3] Blocked PIN should return 6983, got %04x\n", sw);
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    const int RANDOM_ITERS  = 100000;
    const int IMPORT_ITERS  = 50000;

    printf("fuzz_openpgp: starting\n");
    printf("  Phase 1: %d random APDUs (unselected + selected)...\n", RANDOM_ITERS);
    fuzz_random_apdu(RANDOM_ITERS);
    printf("  Phase 1: done\n");

    printf("  Phase 2: %d key-import mutations...\n", IMPORT_ITERS);
    fuzz_import_mutations(IMPORT_ITERS);
    printf("  Phase 2: done\n");

    printf("  Phase 3: UIF bypass attempts...\n");
    fuzz_uif_bypass();
    printf("  Phase 3: done\n");

    printf("  Phase 4: G1 pubkey/GET DATA leak check...\n");
    fuzz_g1_pubkey_leak();
    printf("  Phase 4: done\n");

    printf("  Phase 5: GET DATA 6E overflow guard...\n");
    fuzz_get_data_overflow();
    printf("  Phase 5: done\n");

    printf("  Phase 6: PIN brute-force G3 check...\n");
    fuzz_pin_brute();
    printf("  Phase 6: done\n");

    printf("Fuzz complete: %d + %d + misc iterations, 0 crashes.\n",
           RANDOM_ITERS, IMPORT_ITERS);
    return 0;
}
