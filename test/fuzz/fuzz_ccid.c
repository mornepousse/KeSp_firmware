/*
 * test/fuzz/fuzz_ccid.c — deterministic ASan/UBSan fuzzer for the CCID
 * transport framing in main/security/ccid.c (the dongle's USB bulk layer
 * that parses host-controlled bytes BEFORE the OpenPGP applet sees an APDU).
 *
 * The existing fuzz_openpgp.c only exercises openpgp_card_apdu() (the applet).
 * This harness drives the FIRST thing host malware hits: the 10-byte CCID
 * header parse, the dwLength clamp, the response builders and the ZLP logic.
 *
 * Threat model: a malicious USB host sends arbitrary bulk-OUT bytes with an
 * arbitrary transfer length (xferred) and arbitrary CCID header fields
 * (msg_type, dwLength that may exceed xferred, runt frames < 10 bytes,
 * dwLength near UINT32_MAX, ...).
 *
 * Invariants verified:
 *   - No OOB read/write on the static s_out_buf / s_in_buf (both 512 B) —
 *     ASan instruments globals, so any overflow aborts.
 *   - The worker never receives a dwLength that lets openpgp_card_apdu read
 *     past s_out_buf — the ccid clamp `dwLength <= avail` must hold.
 *   - Any response length the driver queues is <= CCID_BUF_SZ (512) —
 *     asserted inside the usbd_edpt_xfer() shim (ccid_host_stubs.c).
 *
 * Faithfulness note on xferred:
 *   ccid.c primes the bulk-OUT endpoint with sizeof(s_out_buf) = 512, so the
 *   USB controller can never report xferred > 512.  ccid.c legitimately trusts
 *   that bound.  We therefore model a host that *tries* to send up to ~600
 *   bytes but whose transfer is truncated by the controller to 512
 *   (xferred = min(host_len, 512)).  Feeding a fabricated xferred > 512 would
 *   be the harness violating the USB contract, not a ccid.c bug, so we don't.
 *   The huge-length surface is instead fuzzed through the dwLength *header*
 *   field (independently, up to 0xFFFFFFFF), which is where the real clamp
 *   lives.
 *
 * Build (from test/fuzz/):
 *   gcc -std=c11 -g -O1 \
 *       -fsanitize=address,undefined \
 *       -DTEST_HOST -DCCID_HOST_FUZZ \
 *       -I. -Iccid_stubs -I../.. -I../../main/security \
 *       fuzz_ccid.c ccid_host_stubs.c \
 *       ../../main/security/ccid.c \
 *       ../../main/security/openpgp_card.c \
 *       ../../main/security/openpgp_do.c \
 *       ../../main/security/apdu.c \
 *       ../../main/security/sec_confirm.c \
 *       -o fuzz_ccid
 *
 * Run:
 *   ./fuzz_ccid
 *
 * On success prints "Fuzz complete: N iterations, 0 crashes" and exits 0.
 * Any ASan/UBSan abort or assert() prints the offending state — a FINDING.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ccid_host_stubs.h"
#include "openpgp_card.h"
#include "openpgp_do.h"
#include "sec_confirm.h"

/* ------------------------------------------------------------------ */
/* Accessors exported by ccid.c under CCID_HOST_FUZZ                   */
/* ------------------------------------------------------------------ */
extern uint8_t  *ccid_host_out_buf(void);
extern uint8_t  *ccid_host_in_buf(void);
extern uint32_t  ccid_host_buf_sz(void);
extern void      ccid_host_set_eps(uint8_t out, uint8_t in);
extern void      ccid_host_reset_state(void);
extern void      ccid_host_dispatch(uint8_t rhport, uint32_t xferred);
extern bool      ccid_host_drv_xfer(uint8_t rhport, uint8_t ep_addr,
                                    xfer_result_t result, uint32_t xferred_bytes);

#define EP_OUT  0x04
#define EP_IN   0x84
#define CCID_HDR_LEN 10
#define CCID_BUF_SZ  512

/* CCID message types (mirrors ccid.c). */
#define PC_TO_RDR_ICC_POWER_ON    0x62
#define PC_TO_RDR_ICC_POWER_OFF   0x63
#define PC_TO_RDR_GET_SLOT_STATUS 0x65
#define PC_TO_RDR_XFR_BLOCK       0x6F

/* ------------------------------------------------------------------ */
/* Fake applet hooks (no mbedtls on host) — mirror fuzz_openpgp.c      */
/* ------------------------------------------------------------------ */
static bool fake_sign(const uint8_t d[32], const uint8_t *hash, uint16_t n,
                      uint8_t *out, uint16_t *out_n)
{
    (void)d; (void)hash; (void)n;
    memset(out, 0x42, 64); *out_n = 64;
    return true;
}
static int fake_confirm(void) { return 1; }   /* always authorise — no WTX loop */
static bool fake_pubkey(uint8_t algo, const uint8_t d[32], uint8_t *out, uint16_t *out_n)
{
    (void)d;
    if (algo == PGP_ALGO_ECDH) { memset(out, 0x55, 32); *out_n = 32; }
    else { out[0] = 0x04; memset(out + 1, 0x55, 64); *out_n = 65; }
    return true;
}
static bool fake_ecdh(const uint8_t d[32], const uint8_t *peer, uint16_t peer_n,
                      uint8_t *out, uint16_t *out_n)
{
    (void)d; (void)peer;
    if (peer_n != 32) return false;
    memset(out, 0x33, 32); *out_n = 32;
    return true;
}
static bool fake_genkey(uint8_t algo, uint8_t d_out[32])
{
    memset(d_out, 0xA0 + (algo & 0x0F), 32);
    return true;
}
static const openpgp_card_hooks_t hooks = {
    .sign = fake_sign, .confirm = fake_confirm, .pubkey = fake_pubkey,
    .ecdh = fake_ecdh, .genkey = fake_genkey,
};

/* ------------------------------------------------------------------ */
/* PRNG (xorshift64, fixed seed)                                      */
/* ------------------------------------------------------------------ */
static uint64_t g_rng = 0xC01DCAFEF00DBA11ull;
static uint64_t rng_next(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}
static uint8_t rng_byte(void) { return (uint8_t)(rng_next() & 0xFF); }
static void rng_fill(uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) b[i] = rng_byte(); }

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static void reset_card(void)
{
    sec_confirm_reset();
    openpgp_card_init(&hooks);
    openpgp_card_factory_reset();
}

static uint8_t pick_msg_type(uint64_t r)
{
    switch (r % 8) {
    case 0: return PC_TO_RDR_ICC_POWER_ON;
    case 1: return PC_TO_RDR_ICC_POWER_OFF;
    case 2: return PC_TO_RDR_GET_SLOT_STATUS;
    case 3: case 4: case 5: return PC_TO_RDR_XFR_BLOCK;  /* weighted: the hot path */
    default: return rng_byte();                          /* unknown / garbage type */
    }
}

static uint32_t pick_dwlength(uint64_t r, uint32_t avail)
{
    switch ((r >> 3) % 8) {
    case 0: return 0;
    case 1: return avail;                       /* exactly the available bytes   */
    case 2: return avail + 1;                   /* one past avail -> clamp        */
    case 3: return 0xFFFFFFFFu;                 /* max -> clamp                   */
    case 4: return (uint32_t)(r >> 16) & 0xFFFF;/* random 16-bit                  */
    case 5: return CCID_BUF_SZ;                 /* == buffer size                 */
    case 6: return CCID_BUF_SZ - CCID_HDR_LEN;  /* == applet out_max bound        */
    default: return (uint32_t)(rng_next());     /* fully random 32-bit            */
    }
}

/* Pick a "host transfer length" then model controller truncation to 512. */
static uint32_t pick_xferred(uint64_t r)
{
    uint32_t host_len;
    switch (r % 11) {
    case 0:  host_len = 0; break;
    case 1:  host_len = 1 + (uint32_t)((r >> 4) % 9); break;   /* runt 1..9     */
    case 2:  host_len = CCID_HDR_LEN; break;                   /* exactly 10    */
    case 3:  host_len = 64; break;
    case 4:  host_len = 128; break;
    case 5:  host_len = 256; break;
    case 6:  host_len = 512; break;
    case 7:  host_len = 11 + (uint32_t)((r >> 4) % 502); break;/* 11..512       */
    case 8:  host_len = 500 + (uint32_t)((r >> 4) % 120); break;/* 500..619     */
    case 9:  host_len = (uint32_t)((r >> 4) % 600); break;     /* 0..599        */
    default: host_len = CCID_HDR_LEN + (uint32_t)((r >> 4) % 16); break; /* tiny payloads */
    }
    return host_len > CCID_BUF_SZ ? CCID_BUF_SZ : host_len;   /* controller cap */
}

/* ------------------------------------------------------------------ */
/* One fuzz iteration                                                 */
/* ------------------------------------------------------------------ */
static void fuzz_one(uint64_t r)
{
    uint8_t  *ob       = ccid_host_out_buf();
    uint32_t  xferred  = pick_xferred(r);

    /* Fill the actually-received region with random bytes. */
    rng_fill(ob, xferred);

    /* Stamp a controlled header on top when a full header arrived. */
    if (xferred >= CCID_HDR_LEN) {
        uint8_t  mt    = pick_msg_type(r);
        uint32_t avail = xferred - CCID_HDR_LEN;
        uint32_t dw    = pick_dwlength(r, avail);
        ob[0] = mt;
        ob[1] = (uint8_t)(dw & 0xFF);
        ob[2] = (uint8_t)((dw >> 8) & 0xFF);
        ob[3] = (uint8_t)((dw >> 16) & 0xFF);
        ob[4] = (uint8_t)((dw >> 24) & 0xFF);
        ob[5] = rng_byte();   /* bSlot */
        ob[6] = rng_byte();   /* bSeq  */
    }

    /* Drive the OUT path (header parse + clamp + dispatch + synchronous worker
     * for XFR_BLOCK).  Half the time go through ccid_drv_xfer (also exercises
     * the result!=SUCCESS warn branch) instead of dispatch directly. */
    if (r & 0x10000) {
        xfer_result_t res = (r & 0x20000) ? XFER_RESULT_FAILED : XFER_RESULT_SUCCESS;
        ccid_host_drv_xfer(0, EP_OUT, res, xferred);
    } else {
        ccid_host_dispatch(0, xferred);
    }

    /* Drive the IN-completion path to exercise the ZLP (% 64) logic and the
     * busy-state teardown.  Use multiples of 64 to hit the ZLP branch, plus
     * random lengths. */
    if (r & 0x40000) {
        uint32_t in_xf;
        switch ((r >> 19) % 5) {
        case 0: in_xf = 0; break;                          /* ZLP completion    */
        case 1: in_xf = 64u * (uint32_t)((r >> 22) % 9); break; /* 0..512 mult 64 */
        case 2: in_xf = 64; break;
        case 3: in_xf = 512; break;
        default: in_xf = (uint32_t)((r >> 22) % 513); break;
        }
        ccid_host_drv_xfer(0, EP_IN, XFER_RESULT_SUCCESS, in_xf);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    const int ITERS = 200000;

    printf("fuzz_ccid: starting (%d iterations, seed=0x%016llx)\n",
           ITERS, (unsigned long long)g_rng);

    reset_card();
    ccid_host_set_eps(EP_OUT, EP_IN);
    ccid_host_reset_state();

    for (int i = 0; i < ITERS; i++) {
        uint64_t r = rng_next();
        fuzz_one(r);

        /* Periodically re-baseline the applet + driver state so the worker
         * keeps reaching live code paths (not stuck terminated / busy). */
        if ((i & 0x3FF) == 0x3FF) {
            reset_card();
            ccid_host_set_eps(EP_OUT, EP_IN);
            ccid_host_reset_state();
        }
    }

    printf("Fuzz complete: %d iterations, 0 crashes. "
           "(max queued response = %u bytes over %llu edpt_xfer calls)\n",
           ITERS, (unsigned)g_ccid_stub_xfer_max,
           (unsigned long long)g_ccid_stub_xfer_calls);
    return 0;
}
