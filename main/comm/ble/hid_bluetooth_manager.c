#include "hid_bluetooth_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_timer.h"
#include "esp_hidd_prf_api.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "keyboard_config.h"


uint16_t hid_conn_id = 0;
bool sec_conn = false;
static bool bt_initialized = false;
static bool mem_released = false;
#define HID_BT_TAG HID_DEMO_TAG
#define CHAR_DECLARATION_SIZE   (sizeof(uint8_t))

/* ── Multi-device state ──────────────────────────────────────────── */
static bt_device_slot_t bt_slots[BT_MAX_DEVICES];
static uint8_t bt_active_slot = 0;
static bool bt_pairing_mode = false;

#include "version.h"
#define HIDD_DEVICE_NAME            GATTS_TAG
#define HID_DEMO_TAG "HID_BL"

/* BLE connection and advertising parameters (overridable via board.h) */
#ifndef BLE_CONN_MIN_INTERVAL
#define BLE_CONN_MIN_INTERVAL   0x0006  /* 7.5 ms */
#endif
#ifndef BLE_CONN_MAX_INTERVAL
#define BLE_CONN_MAX_INTERVAL   0x0010  /* 20 ms */
#endif
#ifndef BLE_ADV_INT_MIN
#define BLE_ADV_INT_MIN         0x20
#endif
#ifndef BLE_ADV_INT_MAX
#define BLE_ADV_INT_MAX         0x30
#endif
#ifndef BLE_APPEARANCE
#define BLE_APPEARANCE          0x03c1  /* HID Keyboard */
#endif

static uint8_t hidd_service_uuid128[] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = BLE_CONN_MIN_INTERVAL,
    .max_interval = BLE_CONN_MAX_INTERVAL,
    .appearance = BLE_APPEARANCE,
    .manufacturer_len = 0,
    .p_manufacturer_data =  NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
    .flag = 0x6,
};

esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min        = BLE_ADV_INT_MIN,
    .adv_int_max        = BLE_ADV_INT_MAX,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch(event) {
        case ESP_HIDD_EVENT_REG_FINISH: {
            if (param->init_finish.state == ESP_HIDD_INIT_OK) {
                esp_ble_gap_set_device_name(HIDD_DEVICE_NAME);
                esp_ble_gap_config_adv_data(&hidd_adv_data);

            }
            break;
        }
        case ESP_BAT_EVENT_REG: {
            break;
        }
        case ESP_HIDD_EVENT_DEINIT_FINISH:
	     break;
		case ESP_HIDD_EVENT_BLE_CONNECT: {
            ESP_LOGI(HID_DEMO_TAG, "ESP_HIDD_EVENT_BLE_CONNECT");
            hid_conn_id = param->connect.conn_id;
            break;
        }
        case ESP_HIDD_EVENT_BLE_DISCONNECT: {
            sec_conn = false;
            ESP_LOGI(HID_DEMO_TAG, "ESP_HIDD_EVENT_BLE_DISCONNECT");
            esp_ble_gap_start_advertising(&hidd_adv_params);
            break;
        }
        case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT: {
            ESP_LOGI(HID_DEMO_TAG, "%s, ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT", __func__);
            ESP_LOG_BUFFER_HEX(HID_DEMO_TAG, param->vendor_write.data, param->vendor_write.length);
            break;
        }
        case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT: {
            ESP_LOGI(HID_DEMO_TAG, "ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT");
            ESP_LOG_BUFFER_HEX(HID_DEMO_TAG, param->led_write.data, param->led_write.length);
            break;
        }
        default:
            break;
    }
    return;
}

void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;
     case ESP_GAP_BLE_SEC_REQ_EVT:
        for(int i = 0; i < ESP_BD_ADDR_LEN; i++) {
             ESP_LOGD(HID_DEMO_TAG, "%x:",param->ble_security.ble_req.bd_addr[i]);
        }
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
	 break;
     case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if(!param->ble_security.auth_cmpl.success) {
            ESP_LOGE(HID_DEMO_TAG, "BLE auth fail reason = 0x%x", param->ble_security.auth_cmpl.fail_reason);
            sec_conn = false;
        } else {
            sec_conn = true;
            /* Store connected device address in active slot */
            memcpy(bt_slots[bt_active_slot].addr, param->ble_security.auth_cmpl.bd_addr, 6);
            bt_slots[bt_active_slot].valid = true;
            bt_pairing_mode = false;
            ESP_LOGI(HID_DEMO_TAG, "BLE bonded to slot %d: %02X:%02X:%02X:%02X:%02X:%02X",
                     bt_active_slot,
                     bt_slots[bt_active_slot].addr[0], bt_slots[bt_active_slot].addr[1],
                     bt_slots[bt_active_slot].addr[2], bt_slots[bt_active_slot].addr[3],
                     bt_slots[bt_active_slot].addr[4], bt_slots[bt_active_slot].addr[5]);
            bt_devices_save();
        }
        break;
    default:
        break;
    }
}

void init_hid_bluetooth(void)
{
    uint64_t _t0 = esp_timer_get_time();
    esp_err_t ret;

    // Initialize NVS.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    if (!mem_released) {
        ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
        mem_released = true;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s initialize controller failed", __func__);
        ESP_LOGW(HID_DEMO_TAG, "init_hid_bluetooth aborted after %llu ms", (unsigned long long)((esp_timer_get_time()-_t0)/1000));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s enable controller failed", __func__);
        ESP_LOGW(HID_DEMO_TAG, "init_hid_bluetooth aborted after %llu ms", (unsigned long long)((esp_timer_get_time()-_t0)/1000));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s init bluedroid failed", __func__);
        ESP_LOGW(HID_DEMO_TAG, "init_hid_bluetooth aborted after %llu ms", (unsigned long long)((esp_timer_get_time()-_t0)/1000));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s init bluedroid failed", __func__);
        ESP_LOGW(HID_DEMO_TAG, "init_hid_bluetooth aborted after %llu ms", (unsigned long long)((esp_timer_get_time()-_t0)/1000));
        return;
    }

    if((ret = esp_hidd_profile_init()) != ESP_OK) {
        ESP_LOGE(HID_DEMO_TAG, "%s init bluedroid failed", __func__);
    }

    ///register the callback function to the gap module
    esp_ble_gap_register_callback(gap_event_handler);
    esp_hidd_register_callbacks(hidd_event_callback);

    /* set the security iocap & auth_req & key size & init key response key parameters to the stack*/
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;     //bonding with peer device after authentication
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;           //set the IO capability to No output No input
    uint8_t key_size = 16;      //the key size should be 7~16 bytes
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    /* If your BLE device act as a Slave, the init_key means you hope which types of key of the master should distribute to you,
    and the response key means which key you can distribute to the Master;
    If your BLE device act as a master, the response key means you hope which types of key of the slave should distribute to you,
    and the init key means which key you can distribute to the slave. */
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    bt_initialized = true;

    uint64_t _t1 = esp_timer_get_time();
    ESP_LOGW(HID_DEMO_TAG, "init_hid_bluetooth finished in %llu ms", (unsigned long long)((_t1-_t0)/1000));

}

void deinit_hid_bluetooth(void)
{
    uint64_t _t0 = esp_timer_get_time();
    esp_err_t ret;

    ESP_LOGI(HID_BT_TAG, "Disabling BLE HID");

    // Force disconnect if connected
    if (hid_bluetooth_is_connected()) {
        ESP_LOGI(HID_BT_TAG, "Closing connection %d", hid_conn_id);
        esp_ble_gatts_close(hid_conn_id, 0); 
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }

    // Stop advertising
    esp_ble_gap_stop_advertising();

    // Deinit HID profile
    ret = esp_hidd_profile_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(HID_BT_TAG, "esp_hidd_profile_deinit failed: %d", ret);
    }

    // Disable and deinit Bluedroid
    ret = esp_bluedroid_disable();
    if (ret != ESP_OK) {
        ESP_LOGW(HID_BT_TAG, "esp_bluedroid_disable failed: %d", ret);
    }
    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(HID_BT_TAG, "esp_bluedroid_deinit failed: %d", ret);
    }

    // Disable and deinit BT controller
    ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        ESP_LOGW(HID_BT_TAG, "esp_bt_controller_disable failed: %d", ret);
    }
    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(HID_BT_TAG, "esp_bt_controller_deinit failed: %d", ret);
    }

    bt_initialized = false;

    ESP_LOGI(HID_BT_TAG, "BLE HID disabled");
    sec_conn = false;
    uint64_t _t1 = esp_timer_get_time();
    ESP_LOGW(HID_BT_TAG, "deinit_hid_bluetooth finished in %llu ms", (unsigned long long)((_t1-_t0)/1000));
}

bool hid_bluetooth_is_initialized(void)
{
    return bt_initialized;
}

bool hid_bluetooth_is_connected(void)
{
    return sec_conn;
}

void send_hid_bl_key(uint8_t modifier, uint8_t keycodes[6])
{
    esp_hidd_send_keyboard_value(hid_conn_id, modifier, keycodes, 6);
}

void send_hid_bl_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    esp_hidd_send_mouse_value(hid_conn_id, buttons, x, y, wheel);
}

void save_bt_state(bool enabled) {
    nvs_handle_t my_handle;
    esp_err_t err;
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(HID_DEMO_TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(my_handle, "bt_enabled", enabled ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(HID_DEMO_TAG, "Error (%s) writing bt_enabled!", esp_err_to_name(err));
    }
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(HID_DEMO_TAG, "Error (%s) committing bt_enabled!", esp_err_to_name(err));
    }
    nvs_close(my_handle);
}

bool load_bt_state(void) {
    nvs_handle_t my_handle;
    esp_err_t err;
    uint8_t enabled = 1; // Default to enabled

    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(HID_DEMO_TAG, "Error (%s) opening NVS handle! Using default (enabled)", esp_err_to_name(err));
        return true;
    }
    err = nvs_get_u8(my_handle, "bt_enabled", &enabled);
    nvs_close(my_handle);
    return (enabled != 0);
}

/* ── Multi-device management ─────────────────────────────────────── */

uint8_t bt_get_active_slot(void) { return bt_active_slot; }

const bt_device_slot_t *bt_get_slot(uint8_t slot) {
    if (slot >= BT_MAX_DEVICES) return NULL;
    return &bt_slots[slot];
}

const char *bt_get_connected_name(void) {
    if (!sec_conn) return "";
    if (bt_slots[bt_active_slot].name[0] != '\0')
        return bt_slots[bt_active_slot].name;
    /* Format address as fallback name */
    static char addr_str[18];
    snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             bt_slots[bt_active_slot].addr[0], bt_slots[bt_active_slot].addr[1],
             bt_slots[bt_active_slot].addr[2], bt_slots[bt_active_slot].addr[3],
             bt_slots[bt_active_slot].addr[4], bt_slots[bt_active_slot].addr[5]);
    return addr_str;
}

void bt_disconnect(void) {
    if (!bt_initialized) return;
    if (hid_bluetooth_is_connected()) {
        ESP_LOGI(HID_BT_TAG, "Disconnecting BLE slot %d", bt_active_slot);
        esp_ble_gatts_close(hid_conn_id, 0);
        sec_conn = false;
    }
    esp_ble_gap_stop_advertising();
}

void bt_start_pairing(void) {
    if (!bt_initialized) return;
    bt_disconnect();
    bt_pairing_mode = true;
    /* Clear current slot to accept new device */
    ESP_LOGI(HID_BT_TAG, "Pairing mode on slot %d", bt_active_slot);
    memset(&bt_slots[bt_active_slot], 0, sizeof(bt_device_slot_t));
    /* Start undirected advertising (any device can connect) */
    esp_ble_gap_start_advertising(&hidd_adv_params);
}

bool hid_bluetooth_is_pairing(void) { return bt_pairing_mode; }

void bt_switch_slot(uint8_t slot) {
    if (slot >= BT_MAX_DEVICES) return;
    if (slot == bt_active_slot && sec_conn) return; /* already on this slot */

    ESP_LOGI(HID_BT_TAG, "Switching to BT slot %d", slot);
    bt_disconnect();
    bt_active_slot = slot;

    if (bt_slots[slot].valid) {
        /* Reconnect to known device via directed advertising */
        esp_ble_adv_params_t directed_params = hidd_adv_params;
        directed_params.adv_type = ADV_TYPE_DIRECT_IND_HIGH;
        memcpy(directed_params.peer_addr, bt_slots[slot].addr, 6);
        directed_params.peer_addr_type = BLE_ADDR_TYPE_PUBLIC;
        esp_ble_gap_start_advertising(&directed_params);

        /* Fallback to undirected after short timeout if directed fails */
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!sec_conn) {
            ESP_LOGW(HID_BT_TAG, "Directed adv failed, falling back to undirected");
            esp_ble_gap_start_advertising(&hidd_adv_params);
        }
    } else {
        /* No bond on this slot — go to pairing mode */
        bt_start_pairing();
    }

    /* Save active slot */
    nvs_handle_t h;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bt_slot", bt_active_slot);
        nvs_commit(h);
        nvs_close(h);
    }
}

void bt_next_device(void) {
    uint8_t next = (bt_active_slot + 1) % BT_MAX_DEVICES;
    bt_switch_slot(next);
}

void bt_prev_device(void) {
    uint8_t prev = (bt_active_slot == 0) ? BT_MAX_DEVICES - 1 : bt_active_slot - 1;
    bt_switch_slot(prev);
}

void bt_devices_save(void) {
    nvs_handle_t h;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bt_slots", bt_slots, sizeof(bt_slots));
        nvs_set_u8(h, "bt_slot", bt_active_slot);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(HID_BT_TAG, "BT device slots saved");
    }
}

void bt_devices_load(void) {
    memset(bt_slots, 0, sizeof(bt_slots));
    nvs_handle_t h;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(bt_slots);
        nvs_get_blob(h, "bt_slots", bt_slots, &sz);
        nvs_get_u8(h, "bt_slot", &bt_active_slot);
        if (bt_active_slot >= BT_MAX_DEVICES) bt_active_slot = 0;
        nvs_close(h);
        ESP_LOGI(HID_BT_TAG, "BT slots loaded, active=%d", bt_active_slot);
    }
}