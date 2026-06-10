/* main/security/ccid.c — TinyUSB CCID class driver (dongle, Phase 1)
 *
 * Phase 1 behaviour:
 *   - PC_to_RDR_IccPowerOn / IccPowerOff / GetSlotStatus are handled inline
 *     (instant; run in tud_task context).
 *   - PC_to_RDR_XfrBlock is offloaded to a dedicated FreeRTOS worker task so
 *     that PSO:CDS + UIF can block up to SEC_CONFIRM_TIMEOUT_MS waiting for a
 *     physical keypress without stalling the USB task (and thereby CDC + HID).
 *     While the worker waits for the touch, the host is kept alive with CCID
 *     time-extension frames (RDR_to_PC_DataBlock bStatus=0x80, bError=0x02).
 *
 * Implements USB CCID Rev 1.1 §6:
 *   - claims the CCID interface (class 0x0B, interface 4 of the dongle
 *     composite descriptor built in usb_hid.c)
 *   - opens the bulk IN/OUT endpoint pair
 *   - reassembles a CCID message (10-byte header + abData) on bulk OUT
 *   - dispatches by bMessageType and queues an RDR_to_PC response on bulk IN
 *
 * One message in flight at a time (bMaxCCIDBusySlots = 1).
 * All buffers static (no malloc). bSlot and bSeq are always echoed.
 *
 * NOTE: openpgp_card_apdu() mutates applet statics (retries, verified flags,
 * s_key).  It is called exclusively from the ccid_worker task — safe for the
 * current single-consumer design.  If openpgp_card_factory_reset() is ever
 * called concurrently (e.g. from an admin CDC command), a mutex shared by
 * both callers will be required.
 *
 * Compiled for the dongle role only (gated in main/CMakeLists.txt).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "device/usbd_pvt.h"   /* usbd_class_driver_t, usbd_*, tu_desc_* */
#include "openpgp_card.h"
#include "openpgp_crypto.h"
#include "sec_confirm.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CCID";

/* ------------------------------------------------------------------ */
/* CCID message types (USB CCID Rev 1.1 §6)                            */
/* ------------------------------------------------------------------ */
#define PC_TO_RDR_ICC_POWER_ON    0x62
#define PC_TO_RDR_ICC_POWER_OFF   0x63
#define PC_TO_RDR_GET_SLOT_STATUS 0x65
#define PC_TO_RDR_XFR_BLOCK       0x6F

#define RDR_TO_PC_DATA_BLOCK      0x80
#define RDR_TO_PC_SLOT_STATUS     0x81

#define CCID_HDR_LEN              10

/* bStatus: bmICCStatus(bits1:0)=0 present+active, bmCommandStatus(bits7:6)=0 ok */
#define CCID_STATUS_OK           0x00
#define CCID_ERROR_NONE          0x00

/* sec_confirm slot ID reserved for OpenPGP UIF.
 * sec_store uses slots 0-3; 0xF0 is a dedicated sentinel for the PGP touch gate. */
#define CCID_CONFIRM_SLOT        0xF0u

/* How often to fire a WTX frame while the worker waits for a UIF touch. */
#define CCID_WTX_PERIOD_MS       1500u

/* Message buffer: 10-byte header + abData. Sized to comfortably hold a
 * short-APDU exchange (dwMaxCCIDMessageLength = 271 in the descriptor) plus
 * margin. No malloc; static buffers only. */
#define CCID_BUF_SZ              512

/* ------------------------------------------------------------------ */
/* ATR for IccPowerOn (OpenPGP-style T=1).                             */
/* The ATR advertises T=1 + T=15 global bytes, so a TCK check byte is  */
/* MANDATORY (ISO 7816-3): TCK = XOR of all bytes from T0 to the last  */
/* historical byte. Here XOR(DA..00) = 0xCD. Without it scdaemon       */
/* rejects the card with "update_param_by_atr failed: -1".             */
/* ------------------------------------------------------------------ */
static const uint8_t s_atr[] = {
    0x3B,0xDA,0x18,0xFF,0x81,0xB1,0xFE,0x75,0x1F,0x03,
    0x00,0x31,0x84,0x73,0x80,0x01,0x80,0x00,0x90,0x00,
    0xCD   /* TCK = XOR(T0..last historical) */
};

/* ------------------------------------------------------------------ */
/* Endpoint addresses                                                   */
/* ------------------------------------------------------------------ */
static uint8_t s_ep_out;   /* bulk OUT endpoint addr (host -> device) */
static uint8_t s_ep_in;    /* bulk IN  endpoint addr (device -> host) */

/* ------------------------------------------------------------------ */
/* APDU buffers (static, no malloc)                                    */
/* ------------------------------------------------------------------ */
static uint8_t s_out_buf[CCID_BUF_SZ];   /* incoming CCID message (OUT)  */
static uint8_t s_in_buf[CCID_BUF_SZ];    /* outgoing CCID response (IN)  */

/* Separate buffer for WTX frames so they never clobber s_in_buf while
 * the worker is building the final APDU response. */
static uint8_t s_wtx_buf[CCID_HDR_LEN];

/* ------------------------------------------------------------------ */
/* Worker-task state                                                    */
/* ------------------------------------------------------------------ */
static TaskHandle_t      s_worker;
static SemaphoreHandle_t s_msg_ready;     /* binary: one XfrBlock pending    */
static volatile bool     s_busy;          /* true while XfrBlock is in flight */

/* XfrBlock context saved by ccid_dispatch for the worker to echo back. */
static uint8_t           s_cur_slot;
static uint8_t           s_cur_seq;
static uint8_t           s_rhport;
static volatile uint16_t s_resp_len;      /* final response length in s_in_buf */

/* Set to true immediately before queueing a final IN response (from either
 * the inline path or ccid_send_final_cb).  Read and cleared in xfer_cb(IN).
 * Both the set and the clear happen in tud_task context — no additional
 * locking needed.  When false, an IN completion is a WTX acknowledgement
 * and OUT must NOT be re-primed yet. */
static volatile bool     s_final_queued;

/* ------------------------------------------------------------------ */
/* Response builders (write into s_in_buf, return total byte count)    */
/* ------------------------------------------------------------------ */

/* Fill the 10-byte RDR_to_PC header. Assumes abData (data_len bytes) is
 * already present at s_in_buf[CCID_HDR_LEN]. */
static uint16_t ccid_fill_header(uint8_t msg_type, uint8_t slot, uint8_t seq,
                                 uint16_t data_len)
{
    s_in_buf[0] = msg_type;
    s_in_buf[1] = (uint8_t)(data_len & 0xFF);
    s_in_buf[2] = (uint8_t)((data_len >> 8) & 0xFF);
    s_in_buf[3] = 0x00;          /* dwLength is LE; payload < 64KB here */
    s_in_buf[4] = 0x00;
    s_in_buf[5] = slot;          /* echo bSlot */
    s_in_buf[6] = seq;           /* echo bSeq  */
    s_in_buf[7] = CCID_STATUS_OK;
    s_in_buf[8] = CCID_ERROR_NONE;
    s_in_buf[9] = 0x00;          /* bChainParameter / bClockStatus */
    return (uint16_t)(CCID_HDR_LEN + data_len);
}

static uint16_t ccid_build_datablock(uint8_t slot, uint8_t seq,
                                     const uint8_t *data, uint16_t data_len)
{
    if (data_len > CCID_BUF_SZ - CCID_HDR_LEN)
        data_len = CCID_BUF_SZ - CCID_HDR_LEN;
    if (data_len && data)
        memcpy(&s_in_buf[CCID_HDR_LEN], data, data_len);
    return ccid_fill_header(RDR_TO_PC_DATA_BLOCK, slot, seq, data_len);
}

static uint16_t ccid_build_slotstatus(uint8_t slot, uint8_t seq)
{
    return ccid_fill_header(RDR_TO_PC_SLOT_STATUS, slot, seq, 0);
}

/* ------------------------------------------------------------------ */
/* USB-task-side callbacks (scheduled via usbd_defer_func)             */
/* These run on the tud_task; only they may call usbd_edpt_xfer.       */
/* ------------------------------------------------------------------ */

/* Send a WTX (time extension) frame to keep scdaemon waiting.
 * Silently skipped if the IN endpoint is already busy (e.g. a previous
 * WTX or the final response is still in flight). */
static void ccid_send_wtx_cb(void *param)
{
    (void)param;
    if (usbd_edpt_busy(s_rhport, s_ep_in)) return;
    s_wtx_buf[0] = RDR_TO_PC_DATA_BLOCK;
    s_wtx_buf[1] = 0x00;   /* dwLength = 0, LE */
    s_wtx_buf[2] = 0x00;
    s_wtx_buf[3] = 0x00;
    s_wtx_buf[4] = 0x00;
    s_wtx_buf[5] = s_cur_slot;
    s_wtx_buf[6] = s_cur_seq;
    s_wtx_buf[7] = 0x80;   /* bmCommandStatus=10b → time extension; bmICCStatus=00b */
    s_wtx_buf[8] = 0x02;   /* bError: BWT multiplier 2 */
    s_wtx_buf[9] = 0x00;
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, s_wtx_buf, CCID_HDR_LEN))
        ESP_LOGW(TAG, "WTX send failed");
}

/* Send the final APDU response built by the worker (s_in_buf[0..s_resp_len)).
 * Retries on the next tud_task tick if a WTX frame is still in flight — this
 * handles the rare race where a WTX was queued just before dongle_confirm()
 * returned and both arrive in the defer queue before the WTX transfer drains. */
static void ccid_send_final_cb(void *param)
{
    uintptr_t retries = (uintptr_t)param;
    if (usbd_edpt_busy(s_rhport, s_ep_in)) {
        if (retries < 200)
            usbd_defer_func(ccid_send_final_cb, (void *)(retries + 1), false);
        else
            ESP_LOGE(TAG, "final send abandoned after 200 retries");
        return;
    }
    s_final_queued = true;
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, s_in_buf, s_resp_len))
        ESP_LOGE(TAG, "final IN queue failed");
}

/* ------------------------------------------------------------------ */
/* CCID worker task                                                    */
/* ------------------------------------------------------------------ */

/* Runs indefinitely; woken by a binary semaphore each time a
 * PC_to_RDR_XfrBlock arrives.  Owns s_in_buf while s_busy is true. */
static void ccid_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_msg_ready, portMAX_DELAY);

        /* dwLength was clamped by ccid_dispatch and written back into
         * s_out_buf[1..4] (bytes 3+4 zeroed) before the semaphore was given. */
        uint32_t dwLength = (uint32_t)s_out_buf[1]
                          | ((uint32_t)s_out_buf[2] << 8);

        uint16_t apdu_n = openpgp_card_apdu(
                              &s_out_buf[CCID_HDR_LEN], (uint16_t)dwLength,
                              &s_in_buf[CCID_HDR_LEN], CCID_BUF_SZ - CCID_HDR_LEN);

        if (apdu_n >= 2) {
            ESP_LOGD(TAG, "APDU in=%u out=%u SW=%02x%02x",
                     (unsigned)dwLength, (unsigned)apdu_n,
                     s_in_buf[CCID_HDR_LEN + apdu_n - 2],
                     s_in_buf[CCID_HDR_LEN + apdu_n - 1]);
        }

        /* One-shot stack high-watermark log (mbedTLS ECDSA P-256 ~2-3 KB). */
        static bool s_hwm_logged;
        if (!s_hwm_logged) {
            s_hwm_logged = true;
            ESP_LOGD(TAG, "worker stack HWM: %u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        s_resp_len = ccid_fill_header(RDR_TO_PC_DATA_BLOCK,
                                      s_cur_slot, s_cur_seq, apdu_n);

        /* s_in_buf and s_resp_len are written by the worker BEFORE usbd_defer_func().
         * The FreeRTOS queue inside usbd_defer_func provides a full memory barrier
         * (SMP spinlock), guaranteeing visibility from tud_task. Do not bypass it. */
        usbd_defer_func(ccid_send_final_cb, NULL, false);
    }
}

/* ------------------------------------------------------------------ */
/* Real applet hooks (Phase 1)                                          */
/* ------------------------------------------------------------------ */

static bool dongle_sign(const uint8_t d[32],
                        const uint8_t *hash, uint16_t n,
                        uint8_t *out, uint16_t *out_n)
{
    if (!openpgp_crypto_p256_sign(d, hash, n, out)) return false;
    *out_n = 64;
    return true;
}

/* UIF gate — runs on the ccid_worker task (NOT tud_task).
 * Arms sec_confirm, then polls every 20 ms.  While waiting, fires a CCID
 * time-extension (WTX) frame every CCID_WTX_PERIOD_MS so scdaemon does not
 * time out.  Returns 1 if authorised by touch, 2 if denied / timed out. */
static int dongle_confirm(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t deadline = now + SEC_CONFIRM_TIMEOUT_MS;
    sec_confirm_arm(CCID_CONFIRM_SLOT, now);
    uint32_t last_wtx = now;
    uint8_t  slot     = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(now - deadline) >= 0) return 2;   /* unconditional termination */

        sec_confirm_state_t st = sec_confirm_poll(now, &slot);
        if (st == SEC_CONFIRM_AUTHORIZED)
            return (slot == CCID_CONFIRM_SLOT) ? 1 : 2;
        if (st == SEC_CONFIRM_TIMEDOUT)
            return 2;

        if (now - last_wtx >= CCID_WTX_PERIOD_MS) {
            usbd_defer_func(ccid_send_wtx_cb, NULL, false);
            last_wtx = now;
        }
    }
}

/* Derive Q = d·G for READ PUBLIC KEY (INS 0x47 P1=0x81) — gpg keytocard reads
 * the card's public key here to build the card-backed secret stub. */
static bool dongle_pubkey(const uint8_t d[32], uint8_t out_pub[65])
{
    return openpgp_crypto_p256_pubkey(d, out_pub);
}

static const openpgp_card_hooks_t s_dongle_hooks = {
    .sign    = dongle_sign,
    .confirm = dongle_confirm,
    .pubkey  = dongle_pubkey,
};

/* ------------------------------------------------------------------ */
/* Message dispatch (runs in tud_task context via ccid_drv_xfer)       */
/* ------------------------------------------------------------------ */
static void ccid_dispatch(uint8_t rhport, uint32_t xferred)
{
    /* Re-prime OUT and bail on a runt frame (no full header).
     * Guard s_busy: if a worker is live, OUT should not be primed
     * (the runt should never happen in that state, but be safe). */
    if (xferred < CCID_HDR_LEN) {
        if (!s_busy) {
            if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf)))
                ESP_LOGE(TAG, "OUT re-prime failed — CCID pipe wedged");
        }
        return;
    }

    uint8_t  msg_type = s_out_buf[0];
    uint32_t dwLength = (uint32_t)s_out_buf[1]
                      | ((uint32_t)s_out_buf[2] << 8)
                      | ((uint32_t)s_out_buf[3] << 16)
                      | ((uint32_t)s_out_buf[4] << 24);
    uint8_t  bSlot = s_out_buf[5];
    uint8_t  bSeq  = s_out_buf[6];

    ESP_LOGD(TAG, "CCID msg type=0x%02x seq=%u dwLen=%u",
             msg_type, bSeq, (unsigned)dwLength);

    /* Clamp the declared payload to what actually arrived. */
    uint32_t avail = xferred - CCID_HDR_LEN;
    if (dwLength > avail) dwLength = avail;

    uint16_t resp_len;
    switch (msg_type) {
    case PC_TO_RDR_ICC_POWER_ON:
        resp_len = ccid_build_datablock(bSlot, bSeq, s_atr, sizeof(s_atr));
        break;

    case PC_TO_RDR_ICC_POWER_OFF:
    case PC_TO_RDR_GET_SLOT_STATUS:
        resp_len = ccid_build_slotstatus(bSlot, bSeq);
        break;

    case PC_TO_RDR_XFR_BLOCK:
        /* Write the clamped dwLength back into s_out_buf so the worker reads
         * the safe value (bytes 3+4 zeroed; max apdu < 64 KB). */
        s_out_buf[1] = (uint8_t)(dwLength & 0xFF);
        s_out_buf[2] = (uint8_t)((dwLength >> 8) & 0xFF);
        s_out_buf[3] = 0x00;
        s_out_buf[4] = 0x00;
        s_cur_slot = bSlot;
        s_cur_seq  = bSeq;
        s_rhport   = rhport;
        s_busy     = true;
        xSemaphoreGive(s_msg_ready);
        return;   /* worker owns s_in_buf, IN endpoint, and OUT re-prime */

    default:
        /* Unhandled message: answer with a slot status so the host is
         * not left waiting. */
        resp_len = ccid_build_slotstatus(bSlot, bSeq);
        break;
    }

    /* Queue the inline response.  Set s_final_queued so xfer_cb knows this
     * IN completion is a final (not WTX) transfer and must re-prime OUT. */
    s_final_queued = true;
    if (!usbd_edpt_xfer(rhport, s_ep_in, s_in_buf, resp_len))
        ESP_LOGE(TAG, "IN queue failed");
}

/* ------------------------------------------------------------------ */
/* Class driver callbacks                                              */
/* ------------------------------------------------------------------ */
static void ccid_drv_init(void)
{
    s_ep_out = 0;
    s_ep_in  = 0;
    /* Wire the real Phase-1 hooks (P-256 sign + sec_confirm UIF gate). */
    openpgp_card_init(&s_dongle_hooks);
}

static bool ccid_drv_deinit(void)
{
    return true;
}

static void ccid_drv_reset(uint8_t rhport)
{
    (void)rhport;
    s_ep_out       = 0;
    s_ep_in        = 0;
    s_busy         = false;
    s_final_queued = false;
}

static uint16_t ccid_drv_open(uint8_t rhport,
                              tusb_desc_interface_t const *desc_intf,
                              uint16_t max_len)
{
    (void)max_len;
    TU_VERIFY(desc_intf->bInterfaceClass == TUSB_CLASS_SMART_CARD, 0);

    uint8_t const *p_desc = tu_desc_next(desc_intf);  /* past 9-byte interface */

    /* Skip the CCID functional (class) descriptor — type 0x21 — and any other
     * non-endpoint descriptors until the first endpoint. */
    while (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT)
        p_desc = tu_desc_next(p_desc);

    uint8_t ep_out = 0, ep_in = 0;
    TU_ASSERT(usbd_open_edpt_pair(rhport, p_desc, 2, TUSB_XFER_BULK,
                                  &ep_out, &ep_in), 0);
    s_ep_out = ep_out;
    s_ep_in  = ep_in;

    /* Prime the first bulk-OUT read. */
    TU_ASSERT(usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf)), 0);

    /* Bytes consumed = interface(9) + CCID class desc(54) + 2*EP(7) = 77.
     * Advance past the two endpoint descriptors and return the delta. */
    p_desc = tu_desc_next(p_desc);   /* past bulk OUT EP */
    p_desc = tu_desc_next(p_desc);   /* past bulk IN  EP */
    return (uint16_t)((uintptr_t)p_desc - (uintptr_t)desc_intf);
}

/* No class-specific control requests are needed; STALL them. */
static bool ccid_drv_control_xfer(uint8_t rhport, uint8_t stage,
                                  tusb_control_request_t const *request)
{
    (void)rhport; (void)stage; (void)request;
    return false;
}

static bool ccid_drv_xfer(uint8_t rhport, uint8_t ep_addr,
                          xfer_result_t result, uint32_t xferred_bytes)
{
    if (result != XFER_RESULT_SUCCESS)
        ESP_LOGW(TAG, "xfer ep=0x%02x result=%d xferred=%u",
                 ep_addr, result, (unsigned)xferred_bytes);

    if (ep_addr == s_ep_out) {
        /* A full CCID message arrived: dispatch it. */
        ccid_dispatch(rhport, xferred_bytes);
        return true;
    }
    if (ep_addr == s_ep_in) {
        if (s_final_queued) {
            /* USB bulk spec §5.8.3: a transfer whose byte count is an exact
             * multiple of MPS does not carry an implicit "end-of-transfer"
             * marker.  The host's libusb bulk read (length = dwMaxCCIDMessageLength
             * = 271) only terminates on a short packet OR a ZLP.  If we just
             * delivered N×64 bytes with no short last packet, send a ZLP now
             * before clearing state and re-priming OUT.  xferred_bytes == 0
             * means the ZLP itself just completed — fall through to re-prime. */
            if (xferred_bytes > 0u && (xferred_bytes % 64u) == 0u) {
                if (!usbd_edpt_xfer(rhport, s_ep_in, NULL, 0)) {
                    ESP_LOGE(TAG, "ZLP send failed — aborting, re-priming OUT");
                    /* ZLP failed: clear state and recover so OUT is not wedged. */
                    s_final_queued = false;
                    s_busy = false;
                    usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf));
                }
                /* Wait for ZLP IN completion (next xfer_cb with xferred_bytes==0). */
                return true;
            }
            /* Final APDU response delivered (short last packet, or ZLP just done).
             * Clear busy state and re-prime OUT so the next command can arrive. */
            s_final_queued = false;
            s_busy = false;
            if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf)))
                ESP_LOGE(TAG, "OUT re-prime failed — CCID pipe wedged");
        }
        /* else: a WTX frame was acknowledged by the host — nothing to do;
         * wait for the next WTX or the final response callback. */
        return true;
    }
    return false;
}

static const usbd_class_driver_t ccid_driver = {
    .name            = "CCID",
    .init            = ccid_drv_init,
    .deinit          = ccid_drv_deinit,
    .reset           = ccid_drv_reset,
    .open            = ccid_drv_open,
    .control_xfer_cb = ccid_drv_control_xfer,
    .xfer_cb         = ccid_drv_xfer,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

/* TinyUSB picks up application class drivers through this weak hook (default
 * empty in usbd.c). esp_tinyusb does not override it, so we provide it. */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &ccid_driver;
}

/* MUST be called from force-linked code (kase_tinyusb_init).
 *
 * usbd_app_driver_get_cb above is a *strong* override of a *weak* default in
 * libtinyusb.a. But ccid.c exports no other referenced symbol, so without this
 * call the linker never pulls ccid.c.obj from libmain.a — the weak (empty)
 * default wins, no CCID app driver is registered, and SET_CONFIGURATION asserts
 * (process_set_config: no driver claims the CCID interface). This reference
 * forces the object in so our strong symbol takes effect. Do not remove.
 *
 * Phase 1 additions: creates the binary semaphore and worker task (once), and
 * runs the crypto self-test (log-only — CCID is not disabled on failure). */
void ccid_init(void)
{
    static bool s_init_done = false;
    if (s_init_done) return;
    s_init_done = true;

    bool ok = openpgp_crypto_selftest();
    ESP_LOGI(TAG, "crypto selftest: %s", ok ? "PASS" : "FAIL");

    s_msg_ready = xSemaphoreCreateBinary();
    configASSERT(s_msg_ready);

    BaseType_t rc = xTaskCreate(ccid_worker, "ccid", 6144, NULL, 5, &s_worker);
    configASSERT(rc == pdPASS);

    ESP_LOGI(TAG, "CCID class driver registered (worker task running)");
}
