/*
 * test/fuzz/ccid_host_stubs.c — implementations for the host CCID shims
 * declared in ccid_host_stubs.h.  Linked into the fuzz_ccid binary only.
 */
#include "ccid_host_stubs.h"
#include "openpgp_crypto.h"   /* fake crypto symbols referenced by ccid.c statics */
#include <string.h>

/* ================================================================== */
/* TinyUSB usbd shims                                                  */
/* ================================================================== */
uint16_t g_ccid_stub_xfer_max   = 0;
uint64_t g_ccid_stub_xfer_calls = 0;

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr,
                    uint8_t *buffer, uint16_t total_bytes)
{
    (void)rhport; (void)ep_addr; (void)buffer;
    /* INVARIANT (CCID_BUF_SZ = 512): the driver must never queue a transfer
     * longer than its static s_in_buf / s_out_buf.  A larger value means a
     * response-length / clamp overflow in ccid.c — a real finding. */
    assert(total_bytes <= 512);
    if (total_bytes > g_ccid_stub_xfer_max) g_ccid_stub_xfer_max = total_bytes;
    g_ccid_stub_xfer_calls++;
    return true;
}

bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport; (void)ep_addr;
    return false;   /* never busy -> response builders always run */
}

void usbd_defer_func(void (*func)(void *), void *param, bool in_isr)
{
    (void)in_isr;
    /* Run synchronously so ccid_send_final_cb (response builder) is exercised. */
    if (func) func(param);
}

bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const *p_desc,
                         uint8_t ep_count, uint8_t xfer_type,
                         uint8_t *ep_out, uint8_t *ep_in)
{
    (void)rhport; (void)p_desc; (void)ep_count; (void)xfer_type;
    if (ep_out) *ep_out = 0x04;
    if (ep_in)  *ep_in  = 0x84;
    return true;
}

/* ================================================================== */
/* FreeRTOS shims                                                      */
/* ================================================================== */
BaseType_t ccid_stub_xTaskCreate(TaskFunction_t fn, const char *name,
                                 uint32_t stack, void *param,
                                 uint32_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)param; (void)prio;
    if (out) *out = (TaskHandle_t)1;
    return pdPASS;
}

void ccid_stub_vTaskDelay(TickType_t ticks) { (void)ticks; }

uint32_t ccid_stub_uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return 4096;
}

SemaphoreHandle_t ccid_stub_xSemaphoreCreateBinary(void)
{
    return (SemaphoreHandle_t)1;
}

BaseType_t ccid_stub_xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s; (void)t;
    return pdTRUE;
}

/* ================================================================== */
/* esp_timer shim                                                      */
/* ================================================================== */
int64_t esp_timer_get_time(void)
{
    static int64_t t = 0;
    t += 1000;   /* +1 ms per call (only used by the unused confirm loop) */
    return t;
}

/* ================================================================== */
/* Fake openpgp_crypto symbols.                                        */
/* These are referenced only by ccid.c's s_dongle_hooks initializer    */
/* (dongle_sign / dongle_pubkey / dongle_ecdh / dongle_genkey) and by  */
/* ccid_init() — none of which the fuzzer ever calls (it installs its  */
/* own fake openpgp_card hooks).  They exist purely to satisfy the     */
/* linker.  Canned, never-secret outputs.                              */
/* ================================================================== */
bool openpgp_crypto_p256_sign(const uint8_t d[32],
                              const uint8_t *hash, uint16_t hash_len,
                              uint8_t sig[64])
{
    (void)d; (void)hash; (void)hash_len;
    memset(sig, 0x11, 64);
    return true;
}

bool openpgp_crypto_selftest(void) { return true; }

bool openpgp_crypto_p256_pubkey(const uint8_t d[32], uint8_t out_pub[65])
{
    (void)d;
    out_pub[0] = 0x04;
    memset(out_pub + 1, 0x22, 64);
    return true;
}

bool openpgp_crypto_x25519_pubkey(const uint8_t d_be[32], uint8_t out_le[32])
{
    (void)d_be;
    memset(out_le, 0x33, 32);
    return true;
}

bool openpgp_crypto_x25519_ecdh(const uint8_t d_be[32],
                                const uint8_t peer_le[32],
                                uint8_t out_le[32])
{
    (void)d_be; (void)peer_le;
    memset(out_le, 0x44, 32);
    return true;
}

bool openpgp_crypto_genkey(uint8_t algo, uint8_t d_out[32])
{
    memset(d_out, 0x55 + (algo & 0x0F), 32);
    return true;
}
