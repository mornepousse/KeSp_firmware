#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "usb_hid.h"
#include "esp_log.h"
#include "i2c_oled_display.h"
#include "cdc_acm_com.h"
#include "keyboard_config.h"
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
#include "otp_proto.h"
#endif

static const char *TAG_UD = "USB_DESCRIPTORS";
volatile uint8_t hid_led_state = 0;

/* TinyUSB descriptors for CDC ACM + HID keyboard combo.
 * Windows-compatible configuration (avoids STATUS_DEVICE_DATA_ERROR). */

/* Endpoints (full USB values, 0x8x = IN, 0x0x = OUT) */
#define EPNUM_CDC_NOTIF   0x81  /* EP1 IN (interrupt) */
#define EPNUM_CDC_OUT     0x02  /* EP2 OUT (bulk) */
#define EPNUM_CDC_IN      0x82  /* EP2 IN  (bulk) */
#define EPNUM_HID         0x83  /* EP3 IN  (interrupt) */
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
#define EPNUM_OTP_HID     0x84  /* EP4 IN  (interrupt) — OTP HID instance 1 */
#define EPNUM_CCID_OUT    0x05  /* EP5 OUT (bulk) — CCID smartcard */
#define EPNUM_CCID_IN     0x85  /* EP5 IN  (bulk) — CCID smartcard */
#endif

/* Explicit device descriptor (overrides TinyUSB default) */
static const tusb_desc_device_t device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,

    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = BOARD_USB_VID,
    .idProduct          = BOARD_USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

/* Total descriptor length for non-dongle boards (CDC + 1 HID) */
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#endif

/* Report IDs to distinguish keyboard and mouse */
enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE = 2,
};
 
/* Report descriptor: Keyboard + Mouse with Report IDs */
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};

/* Configuration descriptor: CDC (2 interfaces) + HID keyboard.
 * Not used on dongle (which has the OTP-extended descriptor below). */
#if !CONFIG_KASE_DEVICE_ROLE_DONGLE
static const uint8_t hid_cdc_configuration_descriptor[] = {
    /* Config 1: 3 interfaces total, 100 mA */
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    /* CDC (Communication + Data): interfaces 0 and 1 */
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8,
                       0x02, 0x82, 64),

    /* HID keyboard boot: interface 2 */
    TUD_HID_DESCRIPTOR(2, 5, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), EPNUM_HID, 16, 1),
};
#endif /* !CONFIG_KASE_DEVICE_ROLE_DONGLE */

#if CONFIG_KASE_DEVICE_ROLE_DONGLE
/* YubiKey OTP HID report descriptor (instance 1):
 * Keyboard-usage collection (page 0x01, usage 0x06) containing an 8-byte
 * feature report.  Feature reports are exchanged over EP0 via SET_REPORT /
 * GET_REPORT class requests; EPNUM_OTP_HID is the required interrupt-IN
 * endpoint that the HID class spec mandates in the interface descriptor. */
static const uint8_t otp_hid_report_descriptor[] = {
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP            ),   /* 0x05, 0x01 */
    HID_USAGE      ( HID_USAGE_DESKTOP_KEYBOARD        ),   /* 0x09, 0x06 */
    HID_COLLECTION ( HID_COLLECTION_APPLICATION        ),   /* 0xA1, 0x01 */
        /* 8-byte vendor feature payload */
        HID_USAGE_PAGE_N ( HID_USAGE_PAGE_VENDOR, 2    ),   /* 0x06, 0x00, 0xFF */
        HID_USAGE        ( 0x01                        ),   /* 0x09, 0x01 */
        HID_LOGICAL_MIN  ( 0x00                        ),   /* 0x15, 0x00 */
        HID_LOGICAL_MAX_N( 0xFF, 2                     ),   /* 0x26, 0xFF, 0x00 */
        HID_REPORT_SIZE  ( 8                           ),   /* 0x75, 0x08 */
        HID_REPORT_COUNT ( 8                           ),   /* 0x95, 0x08 */
        HID_FEATURE      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ), /* 0xB1, 0x02 */
    HID_COLLECTION_END                                      /* 0xC0 */
};

/* CCID functional descriptor (USB CCID Rev 1.1 §5.1) — single slot, T=1, short+extended APDU.
 * Cross-check every field against the spec and a real reader (e.g. a YubiKey) before trusting. */
#define KASE_CCID_DESC \
    0x36, 0x21,                 /* bLength=54, bDescriptorType=0x21 (CCID) */ \
    0x10, 0x01,                 /* bcdCCID = 1.10 */ \
    0x00,                       /* bMaxSlotIndex = 0 (one slot) */ \
    0x07,                       /* bVoltageSupport = 5.0/3.0/1.8 */ \
    0x02, 0x00, 0x00, 0x00,     /* dwProtocols = T=1 */ \
    0xA0, 0x0F, 0x00, 0x00,     /* dwDefaultClock = 4000 kHz */ \
    0xA0, 0x0F, 0x00, 0x00,     /* dwMaximumClock = 4000 kHz */ \
    0x00,                       /* bNumClockSupported = 0 (only default) */ \
    0x80, 0x25, 0x00, 0x00,     /* dwDataRate = 9600 */ \
    0x80, 0x25, 0x00, 0x00,     /* dwMaxDataRate = 9600 */ \
    0x00,                       /* bNumDataRatesSupported = 0 */ \
    0xFE, 0x00, 0x00, 0x00,     /* dwMaxIFSD = 254 */ \
    0x00, 0x00, 0x00, 0x00,     /* dwSynchProtocols = 0 */ \
    0x00, 0x00, 0x00, 0x00,     /* dwMechanical = 0 */ \
    0x40, 0x08, 0x04, 0x00,     /* dwFeatures = 0x00040840 (LE): 0x00000040 automatic
                                 * parameters negotiation + 0x00040000 short+extended
                                 * APDU level exchange (the bit scdaemon requires). This
                                 * is the field scdaemon is pickiest about and the #1
                                 * de-risk knob: if gpg --card-status does not enumerate
                                 * the reader, tune this first — cross-check vs a real
                                 * YubiKey (lsusb -v) and USB CCID Rev 1.1 §5.1. */ \
    0x0F, 0x01, 0x00, 0x00,     /* dwMaxCCIDMessageLength = 271 (short) — raise for extended */ \
    0xFF,                       /* bClassGetResponse = echo */ \
    0xFF,                       /* bClassEnvelope = echo */ \
    0x00, 0x00,                 /* wLcdLayout = none */ \
    0x00,                       /* bPINSupport = 0 (no pinpad; PIN over APDU) */ \
    0x01                        /* bMaxCCIDBusySlots = 1 */

/* Interface 4: CCID smartcard reader (class 0x0B). No TinyUSB macro exists. */
#define TUD_CCID_DESC_LEN (9 + 54 + 7 + 7)   /* itf + CCID class desc + 2 EP */
#define KASE_CCID_ITF_DESC(_itfnum, _stridx, _epout, _epin) \
    /* Interface descriptor */ \
    /* Interface: bNumEndpoints=2, class 0x0B (CCID), subclass 0x00, \
     * bInterfaceProtocol=0x00 (bulk; 0x01/0x02 would be ICCD) */ \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 2, TUSB_CLASS_SMART_CARD, 0x00, 0x00, _stridx, \
    /* CCID functional/class descriptor (54 bytes) */ \
    KASE_CCID_DESC, \
    /* Bulk OUT endpoint */ \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0, \
    /* Bulk IN endpoint */ \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0

/* Total config-descriptor length for dongle: CDC + HID kbd/mouse + HID OTP + CCID */
#define TUSB_DESC_TOTAL_LEN_OTP \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN \
     + TUD_CCID_DESC_LEN)

/* Configuration descriptor for dongle (5 interfaces):
 *   0 = CDC Communication, 1 = CDC Data, 2 = HID kbd/mouse, 3 = HID OTP, 4 = CCID */
static const uint8_t hid_cdc_otp_configuration_descriptor[] = {
    /* Config 1: 5 interfaces total, 100 mA */
    TUD_CONFIG_DESCRIPTOR(1, 5, 0, TUSB_DESC_TOTAL_LEN_OTP,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* CDC (Communication + Data): interfaces 0 and 1 */
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    /* HID keyboard boot: interface 2 (existing kbd/mouse instance 0) */
    TUD_HID_DESCRIPTOR(2, 5, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_report_descriptor), EPNUM_HID, 16, 1),
    /* HID OTP: interface 3 (instance 1, no boot protocol) */
    TUD_HID_DESCRIPTOR(3, 6, HID_ITF_PROTOCOL_NONE,
                       sizeof(otp_hid_report_descriptor), EPNUM_OTP_HID, 8, 5),
    /* CCID smartcard reader: interface 4 */
    KASE_CCID_ITF_DESC(4, 7, EPNUM_CCID_OUT, EPNUM_CCID_IN),
};

/* Extended string table for dongle (index 6 = OTP interface name) */
static const char *hid_string_descriptor_otp[] = {
    (const char[]){ 0x09, 0x04 }, /* 0: Language = English (US) 0x0409 */
    MANUFACTURER_NAME,            /* 1: Manufacturer */
    PRODUCT_NAME,                 /* 2: Product */
    SERIAL_NUMBER,                /* 3: Serial */
    PRODUCT_NAME " CDC",          /* 4: CDC serial port interface */
    PRODUCT_NAME " Keyboard",     /* 5: HID keyboard interface */
    PRODUCT_NAME " OTP",          /* 6: HID OTP interface */
    PRODUCT_NAME " CCID",         /* 7: CCID smartcard interface */
};
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

/* String descriptor table (language + manufacturer + product + serial + interfaces) */
const char *hid_string_descriptor[] = {
    (const char[]){ 0x09, 0x04 }, /* 0: Language = English (US) 0x0409 */
    MANUFACTURER_NAME,            /* 1: Manufacturer */
    PRODUCT_NAME,                 /* 2: Product */
    SERIAL_NUMBER,                /* 3: Serial */
    PRODUCT_NAME " CDC",          /* 4: CDC serial port interface */
    PRODUCT_NAME " Keyboard",     /* 5: HID keyboard interface */
};

/********* TinyUSB HID callbacks ***************/

// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long
// enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    if (instance == 1) {
        return otp_hid_report_descriptor;
    }
#endif
    (void)instance;
    return hid_report_descriptor;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    if (instance == 1 && report_type == HID_REPORT_TYPE_FEATURE) {
        (void)report_id;
        (void)reqlen;
        otp_proto_on_read(buffer);
        return OTP_FEATURE_RPT_SIZE;
    }
#endif
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    if (instance == 1 && report_type == HID_REPORT_TYPE_FEATURE) {
        (void)report_id;
        if (bufsize >= OTP_FEATURE_RPT_SIZE) {
            otp_proto_on_write(buffer);
        }
        return;
    }
#endif
    (void)instance;
    if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
        /* LED report: bit0=NumLock, bit1=CapsLock, bit2=ScrollLock */
        extern volatile uint8_t hid_led_state;
        hid_led_state = buffer[0];
    }
}

/********* Application ***************/

typedef enum {
  MOUSE_DIR_RIGHT,
  MOUSE_DIR_DOWN,
  MOUSE_DIR_LEFT,
  MOUSE_DIR_UP,
  MOUSE_DIR_MAX,
} mouse_dir_t;

 static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    /* initialization */
    size_t rx_size = 0;

    /* read */
    esp_err_t ret = tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK) {
        buf[rx_size] = '\0';
        ESP_LOGI(TAG_UD, "Got data (%d bytes): %s", rx_size, buf);
    } else {
        ESP_LOGE(TAG_UD, "Read error");
    }

    ESP_LOGI(TAG_UD, "Calling receive_data : %d", rx_size);
    receive_data((char*)buf, rx_size);

}

void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rst = event->line_state_changed_data.rts;
    ESP_LOGI(TAG_UD, "Line state changed! dtr:%d, rst:%d", dtr, rst);
}

void tinyusb_cdc_acm_init(void)
{

    tinyusb_config_cdcacm_t amc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&amc_cfg));
    ESP_LOGI(TAG_UD, "CDC ACM initialization DONE");
}

void tinyusb_hid_init(void)
{
    ESP_LOGI(TAG_UD, "USB initialization");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &device_descriptor;
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    /* Dongle: use extended descriptor with OTP HID interface (instance 1) */
    tusb_cfg.descriptor.string = hid_string_descriptor_otp;
    tusb_cfg.descriptor.string_count =
        sizeof(hid_string_descriptor_otp) / sizeof(hid_string_descriptor_otp[0]);
    tusb_cfg.descriptor.full_speed_config = hid_cdc_otp_configuration_descriptor;
#else
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count =
        sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
    tusb_cfg.descriptor.full_speed_config = hid_cdc_configuration_descriptor;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG_UD, "USB initialization DONE");
}

void kase_tinyusb_init(void)
{
    tinyusb_hid_init();
    tinyusb_cdc_acm_init();
}