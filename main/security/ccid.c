/* main/security/ccid.c — minimal TinyUSB CCID class driver (dongle, Phase 0)
 *
 * Implements just enough of USB CCID Rev 1.1 §6 to let scdaemon/gpg talk to
 * the OpenPGP applet (openpgp_card.c):
 *   - claims the CCID interface (class 0x0B, interface 4 of the dongle
 *     composite descriptor built in usb_hid.c),
 *   - opens the bulk IN/OUT endpoint pair,
 *   - reassembles a CCID message (10-byte header + abData) on bulk OUT,
 *   - dispatches by bMessageType and queues an RDR_to_PC response on bulk IN.
 *
 * Message handling (Phase 0 minimum):
 *   PC_to_RDR_IccPowerOn   (0x62) -> RDR_to_PC_DataBlock  (0x80) carrying ATR
 *   PC_to_RDR_IccPowerOff  (0x63) -> RDR_to_PC_SlotStatus (0x81)
 *   PC_to_RDR_GetSlotStatus(0x65) -> RDR_to_PC_SlotStatus (0x81), present+active
 *   PC_to_RDR_XfrBlock     (0x6F) -> openpgp_card_apdu(), wrapped in DataBlock
 *
 * One message in flight at a time (bMaxCCIDBusySlots = 1). All buffers static
 * (no malloc). bSlot and bSeq are always echoed from request to response.
 *
 * Compiled for the dongle role only (gated in main/CMakeLists.txt).
 */
#include "device/usbd_pvt.h"   /* usbd_class_driver_t, usbd_*, tu_desc_* */
#include "openpgp_card.h"
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

/* Message buffer: 10-byte header + abData. Sized to comfortably hold a
 * short-APDU exchange (dwMaxCCIDMessageLength = 271 in the descriptor) plus
 * margin. No malloc; static buffers only. */
#define CCID_BUF_SZ              512

/* ------------------------------------------------------------------ */
/* ATR for IccPowerOn (OpenPGP-style T=1).                                      */
/* The ATR advertises T=1 + T=15 global bytes, so a TCK check byte is           */
/* MANDATORY (ISO 7816-3): TCK = XOR of all bytes from T0 to the last           */
/* historical byte. Here XOR(DA..00) = 0xCD. Without it scdaemon rejects the    */
/* card with "update_param_by_atr failed: -1" (validated on hardware).          */
/* ------------------------------------------------------------------ */
static const uint8_t s_atr[] = {
    0x3B,0xDA,0x18,0xFF,0x81,0xB1,0xFE,0x75,0x1F,0x03,
    0x00,0x31,0x84,0x73,0x80,0x01,0x80,0x00,0x90,0x00,
    0xCD   /* TCK = XOR(T0..last historical) */
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static uint8_t s_ep_out;   /* bulk OUT endpoint addr (host -> device) */
static uint8_t s_ep_in;    /* bulk IN  endpoint addr (device -> host) */

static uint8_t s_out_buf[CCID_BUF_SZ];   /* incoming CCID message      */
static uint8_t s_in_buf[CCID_BUF_SZ];    /* outgoing CCID response     */

/* ------------------------------------------------------------------ */
/* Phase-0 stub applet hooks.                                          */
/*                                                                     */
/* TEMPORARY: SELECT (the de-risk target) needs neither sign nor       */
/* confirm, so these are placeholders. Real crypto (openpgp_crypto /   */
/* mbedtls) and the sec_confirm touch gate are wired in Phase 1.       */
/* ------------------------------------------------------------------ */
static bool phase0_sign(const uint8_t *hash, uint16_t n,
                        uint8_t *out, uint16_t *out_n)
{
    (void)hash; (void)n; (void)out;
    if (out_n) *out_n = 0;
    return false;            /* no crypto yet */
}

static int phase0_confirm(void)
{
    return 1;                /* authorized (placeholder) */
}

static const openpgp_card_hooks_t s_phase0_hooks = {
    .sign    = phase0_sign,
    .confirm = phase0_confirm,
};

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
/* Message dispatch                                                    */
/* ------------------------------------------------------------------ */
static void ccid_dispatch(uint8_t rhport, uint32_t xferred)
{
    /* Re-prime OUT and bail on a runt frame (no full header). */
    if (xferred < CCID_HDR_LEN) {
        if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf)))
            ESP_LOGE(TAG, "OUT re-prime failed — CCID pipe wedged");
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

    case PC_TO_RDR_XFR_BLOCK: {
        /* abData IN is the C-APDU. Feed it to the applet, writing the
         * R-APDU straight into the response abData region; then frame it. */
        uint16_t apdu_n = openpgp_card_apdu(&s_out_buf[CCID_HDR_LEN],
                                            (uint16_t)dwLength,
                                            &s_in_buf[CCID_HDR_LEN],
                                            CCID_BUF_SZ - CCID_HDR_LEN);
        /* apdu_n is always >= 2 (R-APDU always carries a 2-byte SW). */
        ESP_LOGD(TAG, "APDU in=%u out=%u SW=%02x%02x",
                 (unsigned)dwLength, (unsigned)apdu_n,
                 s_in_buf[10 + apdu_n - 2], s_in_buf[10 + apdu_n - 1]);
        resp_len = ccid_fill_header(RDR_TO_PC_DATA_BLOCK, bSlot, bSeq, apdu_n);
        break;
    }

    default:
        /* Unhandled message: answer with a slot status so the host is not
         * left waiting. */
        resp_len = ccid_build_slotstatus(bSlot, bSeq);
        break;
    }

    /* Queue the response. The next OUT read is re-primed once the IN
     * transfer completes (xfer_cb on s_ep_in) — one message in flight. */
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
    /* Wire the OpenPGP applet once, at stack init. Phase-0 stub hooks. */
    openpgp_card_init(&s_phase0_hooks);
}

static bool ccid_drv_deinit(void)
{
    return true;
}

static void ccid_drv_reset(uint8_t rhport)
{
    (void)rhport;
    s_ep_out = 0;
    s_ep_in  = 0;
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

/* No class-specific control requests are needed for Phase 0; STALL them. */
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
        /* A full CCID message arrived: dispatch and queue the response. */
        ccid_dispatch(rhport, xferred_bytes);
        return true;
    }
    if (ep_addr == s_ep_in) {
        /* Response delivered: re-prime the next OUT read. */
        if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf)))
            ESP_LOGE(TAG, "OUT re-prime failed — CCID pipe wedged");
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
 * forces the object in so our strong symbol takes effect. Do not remove. */
void ccid_init(void)
{
    ESP_LOGI(TAG, "CCID class driver registered (app driver hook linked)");
}
