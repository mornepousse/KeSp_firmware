#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "usb_hid.h"
#include "esp_log.h"
#include "i2c_oled_display.h"
#include "cdc_acm_com.h"
#include "keyboard_config.h"

static const char *TAG_UD = "USB_DESCRIPTORS";
volatile uint8_t hid_led_state = 0;

/* TinyUSB descriptors for CDC ACM + HID keyboard combo.
 * Windows-compatible configuration (avoids STATUS_DEVICE_DATA_ERROR). */

/* Endpoints (full USB values, 0x8x = IN, 0x0x = OUT) */
#define EPNUM_CDC_NOTIF   0x81  /* EP1 IN (interrupt) */
#define EPNUM_CDC_OUT     0x02  /* EP2 OUT (bulk) */
#define EPNUM_CDC_IN      0x82  /* EP2 IN  (bulk) */
#define EPNUM_HID         0x83  /* EP3 IN  (interrupt) */

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

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

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

/* Configuration descriptor: CDC (2 interfaces) + HID keyboard */
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
  // We use only one interface and one HID report descriptor, so we can ignore
  // parameter 'instance'
  return hid_report_descriptor;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
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
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
    tusb_cfg.descriptor.full_speed_config = hid_cdc_configuration_descriptor;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG_UD, "USB initialization DONE");
}

void kase_tinyusb_init(void)
{
    tinyusb_hid_init();
    tinyusb_cdc_acm_init();
}