#include "status_display.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_manager.h"
#include "i2c_oled_display.h"
#include "keyboard_config.h"
#ifdef VERSION_1
#include "round_ui.h"
#include "spi_round_display.h"
#else
#include "img_bluetooth.c"
#include "img_usb.c"
#include "img_signal.c"
#endif
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "keymap.h"
#include "esp_log.h"
/* Optional debug: create a minimal UI (only the layer label) to check if allocations cause crashes. Set to 1 to enable. */
#ifndef STATUS_DISPLAY_MINIMAL
#define STATUS_DISPLAY_MINIMAL 0
#endif

#include "matrix.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

LV_FONT_DECLARE(lv_font_montserrat_28);

#ifndef VERSION_1
/* Helper: log LVGL's dynamic memory state for diagnostics */
static void status_display_log_lv_mem(const char *prefix)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI("STATUS_DISP", "%s LV_MEM free=%u biggest=%u used_pct=%u frag_pct=%u", prefix, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size, (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
}

static void status_display_log_heap_info(const char *prefix)
{
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI("STATUS_DISP", "%s Heap free: 8bit=%u, 32bit=%u, largest8=%u", prefix, (unsigned)free8, (unsigned)free32, (unsigned)largest8);
}

static int last_bt_state = -1;   // -1 = inconnu, 0 = OFF, 1 = ON, 2 = JUSTE BT
static int last_path_state = -1; // 0 = USB, 1 = BLE

static lv_obj_t *icon_bt = NULL;
static lv_obj_t *icon_path = NULL;
static lv_obj_t *indicator_mouse = NULL;
static lv_obj_t *label_nrf_debug = NULL;
static lv_obj_t *label_layer_name = NULL; /* persistent label for layout name - avoid recreating each update */
static TickType_t last_mouse_activity = 0;

static const char *status_display_version_text = GATTS_TAG;
static bool bt_blink_visible = true;
static TickType_t bt_blink_last_tick = 0;
static const TickType_t bt_blink_interval_ticks = pdMS_TO_TICKS(500);
#endif /* !VERSION_1 */

static bool status_display_initialized = false;
static bool status_display_sleeping = false;

/* Expose a controlled way to disable the display from other modules */
void status_display_force_disable(void)
{
    /* The display availability flag lives in i2c_oled_display.c */
    extern bool display_available;
    display_available = false;
    status_display_initialized = false;
}

static bool is_showing_splash = false;
static TickType_t splash_start_tick = 0;

/* LVGL lock functions will be used (lvgl_port_lock / lvgl_port_unlock) provided by the LVGL port */


/* Request wake flag so wake work runs in display task context */
/* Extern visible flag for display task loop */
volatile bool request_wake_request = false;

#ifndef VERSION_1
static void status_display_prepare_ui(bool clear_screen);
static void status_display_update_connection_icons(bool force);
static void status_display_init_icons(void);
static void status_display_show_version_splash(void);
#endif
void status_display_show_DFU_prog(void);

#ifdef VERSION_1
    #define UI_SCALE 2
    #define UI_FONT &lv_font_montserrat_28
#else
    #define UI_SCALE 1
    #define UI_FONT &lv_font_montserrat_14
#endif

void draw_separator_line(void)
{
#ifdef VERSION_1
    /* Not used for round display */
    return;
#else
    if(display_available == false) return;
    draw_rectangle_White(0, 37 * UI_SCALE, 128 * UI_SCALE, 2 * UI_SCALE);
#endif
}

void status_display_update_layer_name(void)
{ 
    if(display_available == false || status_display_sleeping) return;
    
#ifdef VERSION_1
    round_ui_update_layer();
#else
    is_showing_splash = false;

    if (!status_display_initialized)
    {
        status_display_prepare_ui(true);
    }
    else
    {
        status_display_prepare_ui(false);
    }

    /* Update persistent label text instead of recreating UI objects */
    if (label_layer_name) {
        if (lvgl_port_lock(100)) {
            if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
                ESP_LOGE("STATUS_DISP", "Heap integrity FAILED before updating layer name - skipping");
                status_display_log_heap_info("layer_name_update");
                lvgl_port_unlock();
                return;
            }
            lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
            lvgl_port_unlock();
        } else {
            ESP_LOGW("STATUS_DISP", "Could not take LVGL port lock to update layer name");
        }
    } else {
        /* Fallback: ensure UI prepared */
        status_display_prepare_ui(false);
    }
#endif
}

void status_display_update(void)
{
    if(display_available == false || status_display_sleeping) return;

#ifdef VERSION_1
    if (is_showing_splash) {
        if ((xTaskGetTickCount() - splash_start_tick) > pdMS_TO_TICKS(3000)) {
            is_showing_splash = false;
            round_ui_refresh_all();
        }
        return;
    }
    round_ui_update();
#else
    uint64_t _t0 = esp_timer_get_time();

    if (is_showing_splash) {
        if ((xTaskGetTickCount() - splash_start_tick) > pdMS_TO_TICKS(3000)) {
            is_showing_splash = false;
            status_display_refresh_all();
        } else {
            uint64_t _t1 = esp_timer_get_time();
            uint64_t _dur = _t1 - _t0;
            if (_dur > 5000) {
                ESP_LOGW("STATUS_DISP", "status_display_update early returned after %llu us", (unsigned long long)_dur);
            }
            return;
        }
    }

    status_display_update_connection_icons(false);

    if (indicator_mouse) {
        if (lvgl_port_lock(50)) {
            if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
                ESP_LOGE("STATUS_DISP", "Heap integrity FAILED before updating indicator_mouse - skipping");
                status_display_log_heap_info("indicator_mouse_update");
                lvgl_port_unlock();
            } else {
                if ((xTaskGetTickCount() - last_mouse_activity) < pdMS_TO_TICKS(200)) {
                    lv_obj_clear_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
                }
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGW("STATUS_DISP", "Could not take LVGL port lock to update indicator_mouse");
        }
    }

    uint64_t _t1 = esp_timer_get_time();
    uint64_t _dur = _t1 - _t0;
    if (_dur > 5000) {
        ESP_LOGW("STATUS_DISP", "status_display_update took %llu us", (unsigned long long)_dur);
    }
#endif
} 

void status_display_start(void)
{
    display_hw_config_t cfg = keyboard_get_display_config();
    
#ifdef VERSION_1
    /* Use dedicated SPI driver for round display */
    if (!spi_display_init(&cfg)) {
        status_display_initialized = false;
        display_available = false;
        return;
    }
    display_available = true;
#else
    /* Use I2C OLED driver */
    display_set_hw_config(&cfg);
    init_display();
    if (!display_available)
    {
        status_display_initialized = false;
        return;
    }
#endif

    /* Silence STATUS_DISP informational logs in normal runs to reduce log noise */
    esp_log_level_set("STATUS_DISP", ESP_LOG_WARN);

    /* LVGL port provides a lock; ensure it's initialized (lvgl_port_init was called in init_display) */

    status_display_sleeping = false;
#ifdef VERSION_1
    round_ui_show_splash(GATTS_TAG);
    is_showing_splash = true;
    splash_start_tick = xTaskGetTickCount();
#else
    status_display_show_version_splash();
#endif
    // status_display_refresh_all();
}

void status_display_refresh_all(void)
{
    if (display_available == false)
        return;

    is_showing_splash = false;
    status_display_sleeping = false;
#ifdef VERSION_1
    round_ui_refresh_all();
#else
    status_display_prepare_ui(true);
    status_display_update_layer_name();
    status_display_update_connection_icons(true);
#endif
}

void status_display_sleep(void)
{
    if (display_available == false || status_display_sleeping)
        return;

#ifdef VERSION_1
    round_ui_sleep();
    status_display_sleeping = true;
    status_display_initialized = false;
#else
    if (lvgl_port_lock(100)) {
        display_clear_screen();
        status_display_sleeping = true;
        icon_bt = NULL;
        icon_path = NULL;
        /* persistent label can be freed by display_clear_screen; reset it to avoid dangling pointer */
        label_layer_name = NULL;
        last_bt_state = -1;
        last_path_state = -1;
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
        status_display_initialized = false;
        lvgl_port_unlock();
    } else {
        ESP_LOGE("STATUS_DISP", "Could not take LVGL port lock to enter sleep - aborting sleep");
    }
#endif /* !VERSION_1 */
}

void status_display_wake(void)
{
    if (display_available == false)
        return;

    if (!status_display_sleeping)
        return;

#ifdef VERSION_1
    round_ui_wake();
    status_display_sleeping = false;
#else
    /* Defer actual UI work to the display task to avoid calling LVGL from other contexts */
    request_wake_request = true;
#endif
}

#ifndef VERSION_1
static void status_display_update_connection_icons(bool force)
{
    if (display_available == false || status_display_sleeping)
        return;
    uint64_t _t0 = esp_timer_get_time();

    int bt_state;
    if (!hid_bluetooth_is_initialized()) {
        bt_state = 0;
    } else if (hid_bluetooth_is_connected()) {
        bt_state = 1;
    } else {
        bt_state = 2;
    }

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;

    if (!force && bt_state == last_bt_state && path_state == last_path_state && bt_state != 2) {
        uint64_t _t1 = esp_timer_get_time();
        uint64_t _dur = _t1 - _t0;
        if (_dur > 5000) {
            ESP_LOGW("STATUS_DISP", "status_display_update_connection_icons short-circuit took %llu us", (unsigned long long)_dur);
        }
        return;
    }

    /* Take LVGL lock and init icons if needed */
    if (!lvgl_port_lock(100)) {
        ESP_LOGW("STATUS_DISP", "Could not take LVGL port lock to update icons");
        return;
    }
    status_display_init_icons();

    /* Re-check heap after init_icons to catch mid-flight corruption */
    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity failed after init_icons - aborting update_connection_icons");
        status_display_log_heap_info("update_conn_icons_after_init");
        lvgl_port_unlock();
        return;
    }

    if (!icon_path) {
        uint64_t _t1 = esp_timer_get_time();
        uint64_t _dur = _t1 - _t0;
        if (_dur > 5000) {
            ESP_LOGW("STATUS_DISP", "status_display_update_connection_icons early exit (no icon) took %llu us", (unsigned long long)_dur);
        }
        lvgl_port_unlock();
        return;
    }

    // Gestion de l'icône BT : visible uniquement si le Bluetooth est initialisé
    if (bt_state == 0) {
        bt_blink_visible = false;
        bt_blink_last_tick = xTaskGetTickCount();
        if (icon_bt) {
            lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (bt_state == 1) {
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
        if (icon_bt) {
            lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
            lv_img_set_src(icon_bt, &wifi);
        }
    } else {
        TickType_t now = xTaskGetTickCount();
        if (force) {
            bt_blink_visible = true;
            bt_blink_last_tick = now;
        } else if ((now - bt_blink_last_tick) >= bt_blink_interval_ticks) {
            bt_blink_visible = !bt_blink_visible;
            bt_blink_last_tick = now;
        }

        if (icon_bt) {
            if (bt_blink_visible) {
                lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
                lv_img_set_src(icon_bt, &wifi);
            } else {
                lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Choix de l'icône chemin (USB/BLE)
    if (path_state == 0) {
        lv_img_set_src(icon_path, &flash);        // USB
    } else {
        lv_img_set_src(icon_path, &bluetooth_16px);         // BLE (on réutilise wifi comme “onde”)
    }

    last_bt_state = bt_state;
    last_path_state = path_state;
    uint64_t _t1 = esp_timer_get_time();
    uint64_t _dur = _t1 - _t0;
    if (_dur > 5000) {
        ESP_LOGW("STATUS_DISP", "status_display_update_connection_icons took %llu us", (unsigned long long)_dur);
    }

    /* Release LVGL lock */
    lvgl_port_unlock();
}


static void status_display_init_icons(void)
{
    if(display_available == false) return;
    if (icon_bt != NULL || icon_path != NULL) return;

    /* Basic heap sanity check before LVGL allocations */
    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity check failed - skipping LVGL allocations");
        return;
    }

    /* Note: caller must hold lvgl_mutex */
    lv_obj_t *scr = lv_scr_act();

    /* Re-check heap before each allocation to catch mid-flight corruption and log progress */
    ESP_LOGI("STATUS_DISP", "init_icons: starting");
    status_display_log_heap_info("init_start");

    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity failed before creating icon_bt - aborting");
        status_display_log_heap_info("icon_bt_fail");
        display_available = false;
        return;
    }
    ESP_LOGI("STATUS_DISP", "init_icons: creating icon_bt");
    status_display_log_heap_info("before_icon_bt");
    status_display_log_lv_mem("before_icon_bt_lv_mem");
    icon_bt = lv_img_create(scr);
    ESP_LOGI("STATUS_DISP", "init_icons: created icon_bt=%p", (void*)icon_bt);
    status_display_log_heap_info("after_icon_bt");
    status_display_log_lv_mem("after_icon_bt_lv_mem");

    ESP_LOGI("STATUS_DISP", "init_icons: setting pos icon_bt");
    status_display_log_heap_info("before_set_pos_icon_bt");
    status_display_log_lv_mem("before_set_pos_icon_bt_lv_mem");
    lv_obj_set_pos(icon_bt, 20 * UI_SCALE, 48 * UI_SCALE);   // bas droite (16x16)
    #ifdef VERSION_1
        lv_img_set_zoom(icon_bt, 512); // 2x zoom (256 is 1x)
    #endif
    status_display_log_heap_info("after_set_pos_icon_bt");
    status_display_log_lv_mem("after_set_pos_icon_bt_lv_mem");

    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity failed before creating icon_path - aborting");
        status_display_log_heap_info("icon_path_fail");
        display_available = false;
        return;
    }
#if !STATUS_DISPLAY_MINIMAL
    ESP_LOGI("STATUS_DISP", "init_icons: creating icon_path");
    status_display_log_heap_info("before_icon_path");
    status_display_log_lv_mem("before_icon_path_lv_mem");
    icon_path = lv_img_create(scr);
    ESP_LOGI("STATUS_DISP", "init_icons: created icon_path=%p", (void*)icon_path);
    status_display_log_heap_info("after_icon_path");
    status_display_log_lv_mem("after_icon_path_lv_mem");
#endif

    ESP_LOGI("STATUS_DISP", "init_icons: setting pos icon_path");
    status_display_log_heap_info("before_set_pos_icon_path");
    status_display_log_lv_mem("before_set_pos_icon_path_lv_mem");
    if (icon_path) {
        lv_obj_set_pos(icon_path, 0, 48 * UI_SCALE);  // bas gauche (16x16)
        #ifdef VERSION_1
            lv_img_set_zoom(icon_path, 512); // 2x zoom
        #endif
    } else {
        ESP_LOGW("STATUS_DISP", "init_icons: icon_path is NULL (STATUS_DISPLAY_MINIMAL?), skipping set_pos");
    }
    status_display_log_heap_info("after_set_pos_icon_path");
    status_display_log_lv_mem("after_set_pos_icon_path_lv_mem");

    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity failed before creating indicator_mouse - aborting");
        status_display_log_heap_info("indicator_mouse_fail");
        display_available = false;
        return;
    }
#if !STATUS_DISPLAY_MINIMAL
    ESP_LOGI("STATUS_DISP", "init_icons: creating indicator_mouse");
    status_display_log_heap_info("before_indicator_mouse");
    status_display_log_lv_mem("before_indicator_mouse_lv_mem");
    indicator_mouse = lv_label_create(scr);
    ESP_LOGI("STATUS_DISP", "init_icons: created indicator_mouse=%p", (void*)indicator_mouse);
    status_display_log_heap_info("after_indicator_mouse");
    status_display_log_lv_mem("after_indicator_mouse_lv_mem");
#endif

    ESP_LOGI("STATUS_DISP", "init_icons: setting label/text for indicator_mouse");
    if (indicator_mouse) {
        lv_label_set_text(indicator_mouse, "M");
        lv_obj_set_style_text_font(indicator_mouse, UI_FONT, 0); // Apply bigger font if needed
        status_display_log_heap_info("after_indicator_mouse_settext");
        ESP_LOGI("STATUS_DISP", "init_icons: setting pos indicator_mouse");
        status_display_log_heap_info("before_set_pos_indicator_mouse");
        status_display_log_lv_mem("before_set_pos_indicator_mouse_lv_mem");
        lv_obj_set_pos(indicator_mouse, 118 * UI_SCALE, 48 * UI_SCALE);
        status_display_log_heap_info("after_set_pos_indicator_mouse");
        status_display_log_lv_mem("after_set_pos_indicator_mouse_lv_mem");
        lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGW("STATUS_DISP", "init_icons: indicator_mouse is NULL (STATUS_DISPLAY_MINIMAL?), skipping settext/pos");
    }

    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity failed before creating label_nrf_debug - aborting");
        status_display_log_heap_info("label_nrf_debug_fail");
        display_available = false;
        return;
    }
#if !STATUS_DISPLAY_MINIMAL
    ESP_LOGI("STATUS_DISP", "init_icons: creating label_nrf_debug");
    status_display_log_heap_info("before_label_nrf_debug");
    status_display_log_lv_mem("before_label_nrf_debug_lv_mem");
    label_nrf_debug = lv_label_create(scr);
    ESP_LOGI("STATUS_DISP", "init_icons: created label_nrf_debug=%p", (void*)label_nrf_debug);
    status_display_log_heap_info("after_label_nrf_debug");
    status_display_log_lv_mem("after_label_nrf_debug_lv_mem");

    /* Double-check LVGL memory integrity before mutating the label */
    if (lv_mem_test() != LV_RES_OK) {
        ESP_LOGE("STATUS_DISP", "lv_mem_test failed before label_nrf_debug settext - aborting");
        status_display_log_lv_mem("lv_mem_test_before_label_nrf_debug");
        display_available = false;
        return;
    }

    lv_label_set_text(label_nrf_debug, "");
    lv_obj_set_style_text_font(label_nrf_debug, UI_FONT, 0);
    status_display_log_heap_info("after_label_nrf_debug_settext");
    status_display_log_lv_mem("after_label_nrf_debug_settext_lv_mem");

    if (lv_mem_test() != LV_RES_OK) {
        ESP_LOGE("STATUS_DISP", "lv_mem_test failed before label_nrf_debug set_pos - aborting");
        status_display_log_lv_mem("lv_mem_test_before_set_pos_label_nrf_debug");
        display_available = false;
        return;
    }

    lv_obj_set_pos(label_nrf_debug, 0, 0);
    status_display_log_heap_info("after_set_pos_label_nrf_debug");
    status_display_log_lv_mem("after_set_pos_label_nrf_debug_lv_mem");
#endif

    /* persistent label for layer name (created once) */
    if (!label_layer_name) {
        if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
            ESP_LOGE("STATUS_DISP", "Heap integrity failed before creating label_layer_name - aborting");
            status_display_log_heap_info("label_layer_name_fail");
            display_available = false;
            return;
        }
        ESP_LOGI("STATUS_DISP", "init_icons: creating label_layer_name");
        status_display_log_heap_info("before_label_layer_name");
        status_display_log_lv_mem("before_label_layer_name_lv_mem");
        label_layer_name = lv_label_create(scr);
        ESP_LOGI("STATUS_DISP", "init_icons: created label_layer_name=%p", (void*)label_layer_name);
        status_display_log_heap_info("after_label_layer_name");
        status_display_log_lv_mem("after_label_layer_name_lv_mem");

        if (lv_mem_test() != LV_RES_OK) {
            ESP_LOGE("STATUS_DISP", "lv_mem_test failed before label_layer_name settext - aborting");
            status_display_log_lv_mem("lv_mem_test_before_label_layer_name");
            display_available = false;
            return;
        }

        lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
        lv_obj_set_style_text_font(label_layer_name, UI_FONT, 0);
        status_display_log_heap_info("after_label_layer_name_settext");
        status_display_log_lv_mem("after_label_layer_name_settext_lv_mem");

        if (lv_mem_test() != LV_RES_OK) {
            ESP_LOGE("STATUS_DISP", "lv_mem_test failed before label_layer_name set_pos - aborting");
            status_display_log_lv_mem("lv_mem_test_before_set_pos_label_layer_name");
            display_available = false;
            return;
        }

        lv_obj_set_pos(label_layer_name, 38 * UI_SCALE, 48 * UI_SCALE);
        status_display_log_heap_info("after_set_pos_label_layer_name");
        status_display_log_lv_mem("after_set_pos_label_layer_name_lv_mem");
    } else {
        status_display_log_heap_info("before_update_label_layer_name");
        status_display_log_lv_mem("before_update_label_layer_name_lv_mem");
        if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
            ESP_LOGE("STATUS_DISP", "Heap integrity FAILED before updating label_layer_name - disabling display");
            status_display_log_heap_info("label_layer_name_update_fail");
            display_available = false;
            return;
        }
        if (lv_mem_test() != LV_RES_OK) {
            ESP_LOGE("STATUS_DISP", "lv_mem_test FAILED before updating label_layer_name - disabling display");
            status_display_log_lv_mem("lv_mem_test_before_label_layer_name_update");
            display_available = false;
            return;
        }

        lv_label_set_text(label_layer_name, default_layout_names[current_layout]);
        lv_obj_set_style_text_font(label_layer_name, UI_FONT, 0);
        status_display_log_heap_info("after_label_layer_name_settext_existing");
        status_display_log_lv_mem("after_label_layer_name_settext_existing_lv_mem");

        if (lv_mem_test() != LV_RES_OK) {
            ESP_LOGE("STATUS_DISP", "lv_mem_test FAILED after settext - disabling display");
            status_display_log_lv_mem("lv_mem_test_after_settext_label_layer_name");
            display_available = false;
            return;
        }

        lv_obj_set_pos(label_layer_name, 38 * UI_SCALE, 48 * UI_SCALE);
        status_display_log_heap_info("after_set_pos_label_layer_name_existing");
        status_display_log_lv_mem("after_set_pos_label_layer_name_existing_lv_mem");
    }
}

static void status_display_prepare_ui(bool clear_screen)
{
    if (display_available == false)
        return;

    if (!lvgl_port_lock(200)) {
        ESP_LOGW("STATUS_DISP", "Could not take LVGL port lock to prepare UI");
        return;
    }

    if (clear_screen)
    {
        display_clear_screen();
        draw_separator_line();
        icon_bt = NULL;
        icon_path = NULL;
        indicator_mouse = NULL;
        label_nrf_debug = NULL;
        /* persistent label can be freed by display_clear_screen; reset it to avoid dangling pointer */
        label_layer_name = NULL;
        last_bt_state = -1;
        last_path_state = -1;
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
    }

    /* Check heap integrity before creating LVGL objects */
    if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
        ESP_LOGE("STATUS_DISP", "Heap integrity FAILED before init_icons - disabling display to avoid crash");
        status_display_log_heap_info("PrepareUI");
        display_available = false;
        lvgl_port_unlock();
        return;
    }

    status_display_init_icons();

    if (!display_available) {
        ESP_LOGW("STATUS_DISP", "Display disabled during init_icons - aborting prepare_ui");
        lvgl_port_unlock();
        return;
    }

    status_display_initialized = true;
    lvgl_port_unlock();
}

static void status_display_show_version_splash(void)
{
    if (display_available == false)
        return;

    display_clear_screen();
    write_text_to_display_centre(status_display_version_text, 0, 0);
    
    is_showing_splash = true;
    splash_start_tick = xTaskGetTickCount();
    status_display_initialized = false;
}
#endif /* !VERSION_1 */


void status_display_show_DFU_prog(void)
{
    if (display_available == false)
        return;
    display_clear_screen();
    write_text_to_display_centre("DFU Mode", 0, 0); 
}

void status_display_notify_mouse_activity(void)
{
#ifdef VERSION_1
    round_ui_notify_mouse();
#else
    last_mouse_activity = xTaskGetTickCount();
#endif
}

void status_display_update_nrf_debug(uint32_t pps, uint8_t status, bool spi_ok, uint8_t rpd, uint8_t last_byte, uint8_t mode)
{
    if (display_available == false || status_display_sleeping) return;

#ifdef VERSION_1
    round_ui_update_nrf_debug(pps, status, spi_ok, rpd, last_byte, mode);
#else
    if (!status_display_initialized) {
        status_display_prepare_ui(false);
    }

    if (label_nrf_debug) {
        if (!lvgl_port_lock(100)) {
            ESP_LOGW("STATUS_DISP", "Could not take LVGL port lock to update nrf debug");
            return;
        }

        if (!heap_caps_check_integrity_all(MALLOC_CAP_DEFAULT)) {
            ESP_LOGE("STATUS_DISP", "Heap integrity FAILED before updating label_nrf_debug - skipping");
            status_display_log_heap_info("label_nrf_debug_update");
            lvgl_port_unlock();
            return;
        }

        if (spi_ok) {
            if (mode == 99) {
                lv_label_set_text(label_nrf_debug, "Scanning...");
            } else if (mode > 0 && mode < 99) {
                lv_label_set_text_fmt(label_nrf_debug, "Found CH:%d", mode);
            } else {
                // Normal Mode
                // Shortened to fit on screen: RX count, Status hex, RPD (Carrier)
                lv_label_set_text_fmt(label_nrf_debug, "RX:%lu S:%02X R:%d", (unsigned long)pps, status, rpd);
            }
        } else {
            lv_label_set_text(label_nrf_debug, "SPI ERROR");
        }

        lvgl_port_unlock();
    }
#endif
}