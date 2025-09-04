#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "usb_descriptors.h"
#include "esp_log.h"
#include "i2c_oled_display.h"
#include "cdc_acm_com.h"
#include "keyboard_config.h"

static const char *TAG_UD = "USB_DESCRIPTORS";

#define EPNUM_CDC_NOTIF 1
#define EPNUM_CDC_IN 2
#define EPNUM_CDC_OUT 2
#define EPNUM_VENDOR_IN 5
#define EPNUM_VENDOR_OUT 5
#define EPNUM_KEYBOARD 4
#define EPNUM_MOUSE 6

#define TUSB_DESC_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
 
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD))
};

static const uint8_t hid_cdc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 2, 0x80 | EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, 0x80 | EPNUM_CDC_IN, 64), // CDC: interface 0
    TUD_HID_DESCRIPTOR(ITF_NUM_TOTAL, 1, 0, sizeof(hid_report_descriptor), 0x80 | EPNUM_KEYBOARD, 16, 10), // HID: interface 1
}; 

const char *hid_string_descriptor[5] = {
    // array of pointer to string descriptors
    (char[]){0x09, 0x04}, // 0: is supported language is English (0x0409)
    MANUFACTURER_NAME,              // 1: Manufacturer
    PRODUCT_NAME,               // 2: Product
    SERIAL_NUMBER,               // 3: Serials, should use chip ID
    "Keyboard",           // 4: HID
};

/**
 * @brief Configuration descriptor
 *
 * This is a simple configuration descriptor that defines 1 configuration and 1
 * HID interface
 */
// static const uint8_t hid_configuration_descriptor[] = { 
//     TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN,
//                           TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100), 
//     TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
// };

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
                           uint16_t bufsize) {}

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

    /* write back */

    //display_clear_screen();
    //display_test_text((char*)buf);

    //tinyusb_cdcacm_write_queue(itf, buf, rx_size);
    //tinyusb_cdcacm_write_flush(itf, 0);
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
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback, // the first way to register a callback
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ESP_ERROR_CHECK(tusb_cdc_acm_init(&amc_cfg));
    ESP_LOGI(TAG_UD, "CDC ACM initialization DONE");
}

void tinyusb_hid_init(void)
{
    ESP_LOGI(TAG_UD, "USB initialization");
   const tinyusb_config_t tusb_cfg = {
       .device_descriptor = NULL,
       .string_descriptor = hid_string_descriptor,
       .string_descriptor_count = 5,
       .external_phy = false,
       .configuration_descriptor = hid_cdc_configuration_descriptor,
   };

  ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
  ESP_LOGI(TAG_UD, "USB initialization DONE");
}

void kase_tinyusb_init(void)
{
    tinyusb_hid_init();
    tinyusb_cdc_acm_init();
}