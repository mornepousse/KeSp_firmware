/**
 * @file round_ui.c
 * @brief Modern circular UI for GC9A01 240x240 round display
 * 
 * Designed for keyboard status display on a round smartwatch-style screen.
 * Uses LVGL symbols and modern dark theme.
 */

#include "round_ui.h"
#include "tamagotchi.h"
#include "tama_engine.h"
#include "tama_render.h"
#include "i2c_oled_display.h"
#include "keyboard_config.h"
#include "hid_bluetooth_manager.h"
#include "keyboard_task.h"
#include "hid_report.h"
#include "keymap.h"
#include "matrix_scan.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ROUND_UI"

#ifndef BT_BLINK_INTERVAL_MS
#define BT_BLINK_INTERVAL_MS    500
#endif
#ifndef SPLASH_DURATION_MS
#define SPLASH_DURATION_MS      3000
#endif
#ifndef MOUSE_INDICATOR_MS
#define MOUSE_INDICATOR_MS      200
#endif

/* Display dimensions */
#define DISP_SIZE BOARD_DISPLAY_WIDTH
#define CENTER_X  (DISP_SIZE / 2)
#define CENTER_Y  (DISP_SIZE / 2)

/* Colors - Modern dark theme with cyan accents */
#define COLOR_BG        lv_color_hex(0x0D1117)   /* Dark background */
#define COLOR_PRIMARY   lv_color_hex(0x58A6FF)   /* Bright blue accent */
#define COLOR_SECONDARY lv_color_hex(0x8B949E)   /* Muted gray */
#define COLOR_SUCCESS   lv_color_hex(0x3FB950)   /* Green for connected */
#define COLOR_WARNING   lv_color_hex(0xF0883E)   /* Orange for pairing */
#define COLOR_TEXT      lv_color_hex(0xE6EDF3)   /* White text */
#define COLOR_TEXT_DIM  lv_color_hex(0x484F58)   /* Dimmed text */
#define COLOR_ARC_BG    lv_color_hex(0x21262D)   /* Arc background */

/* UI Elements */
static lv_obj_t *main_container = NULL;
static lv_obj_t *outer_arc = NULL;
static lv_obj_t *layer_label = NULL;
static lv_obj_t *status_icon = NULL;
static lv_obj_t *conn_icon = NULL;
static lv_obj_t *mouse_indicator = NULL;
/* Animation */
static lv_anim_t arc_anim;
static bool is_animating = false;

/* State tracking */
static bool ui_initialized = false;
static bool ui_sleeping = false;
static bool ui_showing_splash = false;  /* Splash screen active - don't update layer */
static int last_bt_state = -1;
static int last_path_state = -1;
static TickType_t last_mouse_activity = 0;
static TickType_t bt_blink_last_tick = 0;
static bool bt_blink_visible = true;

/* KPM (Keys Per Minute) tracking */
#ifndef KPM_WINDOW_SIZE
#define KPM_WINDOW_SIZE 60
#endif
#ifndef KPM_SAMPLE_INTERVAL_MS
#define KPM_SAMPLE_INTERVAL_MS 1000
#endif
#ifndef KPM_MAX_DISPLAY
#define KPM_MAX_DISPLAY 400
#endif
static uint32_t keypress_count = 0;  /* Total keypresses since last sample */
static uint32_t kpm_history[KPM_WINDOW_SIZE];  /* Rolling history of keypresses per sample */
static int kpm_history_index = 0;
uint32_t current_kpm = 0;  /* Exported for LED strip KPM bar animation */
static TickType_t last_kpm_sample = 0;
static lv_obj_t *kpm_label = NULL;  /* Label to show KPM value */

/* Forward declarations */
static void create_main_ui(void);
static void update_connection_status(bool force);
static void arc_anim_cb(void *var, int32_t val);

/* Font declarations */
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_14);

/**
 * @brief Apply dark theme to the screen
 */
static void apply_dark_theme(lv_obj_t *scr)
{
    /* Set black background */
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    /* Remove default border/padding */
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
}

/**
 * @brief Create the decorative outer arc
 */
static void create_outer_arc(lv_obj_t *parent)
{
    outer_arc = lv_arc_create(parent);
    lv_obj_set_size(outer_arc, 230, 230);
    lv_obj_center(outer_arc);
    
    /* Style the arc */
    lv_obj_set_style_arc_width(outer_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(outer_arc, COLOR_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(outer_arc, true, LV_PART_MAIN);
    
    lv_obj_set_style_arc_width(outer_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(outer_arc, COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(outer_arc, true, LV_PART_INDICATOR);
    
    /* Remove knob */
    lv_obj_set_style_bg_opa(outer_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    
    /* Set arc range and initial value */
    lv_arc_set_range(outer_arc, 0, 100);
    lv_arc_set_value(outer_arc, 5);  /* Start at minimum */
    lv_arc_set_bg_angles(outer_arc, 135, 45);  /* 270 degree arc */
    lv_arc_set_rotation(outer_arc, 0);
    
    /* Disable interaction */
    lv_obj_clear_flag(outer_arc, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief Create connection status icon (BT or USB)
 */
static void create_status_icons(lv_obj_t *parent)
{
    /* USB/BLE indicator — top left */
    conn_icon = lv_label_create(parent);
    lv_obj_set_style_text_font(conn_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(conn_icon, COLOR_PRIMARY, 0);
    lv_label_set_text(conn_icon, LV_SYMBOL_USB);
    lv_obj_align(conn_icon, LV_ALIGN_TOP_MID, -25, 28);

    /* Bluetooth status icon — top right */
    status_icon = lv_label_create(parent);
    lv_obj_set_style_text_font(status_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_icon, COLOR_SUCCESS, 0);
    lv_label_set_text(status_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(status_icon, LV_ALIGN_TOP_MID, 25, 28);
    lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Create center layer name display
 */
static void create_layer_display(lv_obj_t *parent)
{
    /* Layer name — top area, smaller to make room for tama */
    layer_label = lv_label_create(parent);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(layer_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(layer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(layer_label, default_layout_names[current_layout]);
    lv_obj_align(layer_label, LV_ALIGN_TOP_MID, 0, 46);
}

/**
 * @brief Create mouse activity indicator
 */
static void create_mouse_indicator(lv_obj_t *parent)
{
    mouse_indicator = lv_label_create(parent);
    lv_obj_set_style_text_font(mouse_indicator, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mouse_indicator, COLOR_WARNING, 0);
    lv_label_set_text(mouse_indicator, LV_SYMBOL_EYE_OPEN);
    lv_obj_align(mouse_indicator, LV_ALIGN_BOTTOM_MID, 30, -40);
    lv_obj_add_flag(mouse_indicator, LV_OBJ_FLAG_HIDDEN);
}


/**
 * @brief Create KPM display label (below layer name)
 */
static void create_kpm_label(lv_obj_t *parent)
{
    /* KPM — bottom area */
    kpm_label = lv_label_create(parent);
    lv_obj_set_style_text_font(kpm_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(kpm_label, COLOR_SECONDARY, 0);
    lv_obj_set_style_text_align(kpm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(kpm_label, "0 KPM");
    lv_obj_align(kpm_label, LV_ALIGN_BOTTOM_MID, -30, -40);
}

/**
 * @brief Create all UI elements
 */
static void create_main_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    apply_dark_theme(scr);
    
    /* Main container */
    main_container = lv_obj_create(scr);
    lv_obj_set_size(main_container, DISP_SIZE, DISP_SIZE);
    lv_obj_center(main_container);
    lv_obj_set_style_bg_opa(main_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_container, 0, 0);
    lv_obj_set_style_pad_all(main_container, 0, 0);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Create UI components */
    create_outer_arc(scr);
    create_status_icons(scr);
    
    /* Old ASCII tama disabled — replaced by tama_engine + tama_render */
    
    create_layer_display(scr);
    create_kpm_label(scr);
    create_mouse_indicator(scr);

    /* Initialize KPM tracking */
    memset(kpm_history, 0, sizeof(kpm_history));
    kpm_history_index = 0;
    keypress_count = 0;
    current_kpm = 0;
    last_kpm_sample = xTaskGetTickCount();
}

/**
 * @brief Update connection status icons
 */
static void update_connection_status(bool force)
{
    if (!display_available || ui_sleeping) return;
    
    int bt_state;
    if (!hid_bluetooth_is_initialized()) {
        bt_state = 0;  /* BT OFF */
    } else if (hid_bluetooth_is_connected()) {
        bt_state = 1;  /* Connected */
    } else {
        bt_state = 2;  /* Advertising */
    }
    
    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;  /* 0=USB, 1=BLE */
    
    if (!force && bt_state == last_bt_state && path_state == last_path_state && bt_state != 2) {
        return;
    }

    if (!lvgl_port_lock(50)) {
        return;
    }
    
    /* Update connection path icon */
    if (conn_icon) {
        if (path_state == 0) {
            lv_label_set_text(conn_icon, LV_SYMBOL_USB);
            lv_obj_set_style_text_color(conn_icon, COLOR_PRIMARY, 0);
        } else {
            lv_label_set_text(conn_icon, LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(conn_icon, COLOR_SUCCESS, 0);
        }
    }
    
    /* Update Bluetooth status icon */
    if (status_icon) {
        if (bt_state == 0) {
            /* BT disabled */
            lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        } else if (bt_state == 1) {
            /* Connected */
            lv_obj_clear_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(status_icon, LV_SYMBOL_BLUETOOTH);
            lv_obj_set_style_text_color(status_icon, COLOR_SUCCESS, 0);
        } else {
            /* Advertising - blink */
            TickType_t now = xTaskGetTickCount();
            if (force) {
                bt_blink_visible = true;
                bt_blink_last_tick = now;
            } else if ((now - bt_blink_last_tick) >= pdMS_TO_TICKS(BT_BLINK_INTERVAL_MS)) {
                bt_blink_visible = !bt_blink_visible;
                bt_blink_last_tick = now;
            }
            
            if (bt_blink_visible) {
                lv_obj_clear_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(status_icon, LV_SYMBOL_BLUETOOTH);
                lv_obj_set_style_text_color(status_icon, COLOR_WARNING, 0);
            } else {
                lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    
    last_bt_state = bt_state;
    last_path_state = path_state;
    
    lvgl_port_unlock();
}

/* ============ Public API ============ */

void round_ui_init(void)
{
    if (!display_available) {
        ESP_LOGW(TAG, "Display not available, skipping UI init");
        return;
    }
    
    ESP_LOGI(TAG, "Initializing round UI");
    
    if (lvgl_port_lock(200)) {
        create_main_ui();
        {
            lv_obj_t *scr = lv_scr_act();
            tama_render_create(scr, BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
        }
        ui_initialized = true;
        ui_sleeping = false;
        ui_showing_splash = false;
        lvgl_port_unlock();
    }
    
    update_connection_status(true);
}

void round_ui_update_layer(void)
{
    if (!display_available || ui_sleeping || ui_showing_splash) {
        return;
    }
    
    if (!ui_initialized) {
        round_ui_init();
    }
    
    if (layer_label && lvgl_port_lock(100)) {
        lv_label_set_text(layer_label, default_layout_names[current_layout]);
        lv_obj_invalidate(layer_label);
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
}

void round_ui_update(void)
{
    if (!display_available || ui_sleeping) return;
    
    update_connection_status(false);
    
    /* Update KPM calculation every sample interval */
    TickType_t now = xTaskGetTickCount();
    if ((now - last_kpm_sample) >= pdMS_TO_TICKS(KPM_SAMPLE_INTERVAL_MS)) {
        /* Store current keypress count in history */
        kpm_history[kpm_history_index] = keypress_count;
        kpm_history_index = (kpm_history_index + 1) % KPM_WINDOW_SIZE;
        keypress_count = 0;
        last_kpm_sample = now;
        
        /* Calculate KPM from history (sum of last 60 samples = keys in last minute) */
        uint32_t total = 0;
        for (int i = 0; i < KPM_WINDOW_SIZE; i++) {
            total += kpm_history[i];
        }
        current_kpm = total;  /* Since we sample every 1s and have 60 samples, total = KPM */
    }
    
    /* Single lock for all UI updates — avoids repeated lock contention */
    if (!lvgl_port_lock(50)) return;

    /* Update arc based on KPM */
    if (outer_arc) {
        int32_t arc_value = (current_kpm * 100) / KPM_MAX_DISPLAY;
        if (arc_value > 100) arc_value = 100;
        if (arc_value < 2) arc_value = 2;
        lv_arc_set_value(outer_arc, arc_value);

        lv_color_t arc_color;
        if (current_kpm > 300)      arc_color = COLOR_SUCCESS;
        else if (current_kpm > 150) arc_color = COLOR_PRIMARY;
        else if (current_kpm > 50)  arc_color = COLOR_WARNING;
        else                        arc_color = COLOR_SECONDARY;
        lv_obj_set_style_arc_color(outer_arc, arc_color, LV_PART_INDICATOR);
    }

    /* Update KPM label */
    if (kpm_label) {
        lv_label_set_text_fmt(kpm_label, "%lu KPM", (unsigned long)current_kpm);
        lv_color_t text_color;
        if (current_kpm > 300)      text_color = COLOR_SUCCESS;
        else if (current_kpm > 150) text_color = COLOR_PRIMARY;
        else                        text_color = COLOR_SECONDARY;
        lv_obj_set_style_text_color(kpm_label, text_color, 0);
    }

    /* Update mouse indicator */
    if (mouse_indicator) {
        if ((xTaskGetTickCount() - last_mouse_activity) < pdMS_TO_TICKS(MOUSE_INDICATOR_MS))
            lv_obj_clear_flag(mouse_indicator, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(mouse_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    /* Update Tamagotchi render only — keypresses fed separately via notify */
    if (tama_engine_is_enabled()) {
        tama_render_update(tama_engine_get_state(), tama_engine_get_stats(), tama_engine_get_critter());
    }

    lvgl_port_unlock();
}

void round_ui_sleep(void)
{
    if (!display_available || ui_sleeping) return;
    
    if (lvgl_port_lock(100)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        
        main_container = NULL;
        outer_arc = NULL;
        layer_label = NULL;
        status_icon = NULL;
        conn_icon = NULL;
        mouse_indicator = NULL;

        kpm_label = NULL;
        
        ui_sleeping = true;
        ui_initialized = false;
        last_bt_state = -1;
        last_path_state = -1;
        
        lvgl_port_unlock();
    }
}

void round_ui_wake(void)
{
    if (!display_available) return;
    if (!ui_sleeping) return;
    
    ui_sleeping = false;
    round_ui_init();
    round_ui_update_layer();
}

void round_ui_refresh_all(void)
{
    if (!display_available) return;
    
    ui_sleeping = false;
    ui_showing_splash = false;  /* End splash mode */
    
    if (lvgl_port_lock(200)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        
        main_container = NULL;
        outer_arc = NULL;
        layer_label = NULL;
        status_icon = NULL;
        conn_icon = NULL;
        mouse_indicator = NULL;
        kpm_label = NULL;
        tama_render_destroy(); /* reset tama render state before rebuild */

        create_main_ui();
        tama_render_create(scr, BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
        ui_initialized = true;
        lvgl_port_unlock();
    }

    round_ui_update_layer();
    update_connection_status(true);
}

void round_ui_notify_mouse(void)
{
    last_mouse_activity = xTaskGetTickCount();
}

void round_ui_notify_keypress(void)
{
    keypress_count++;
    if (tama_engine_is_enabled())
        tama_engine_keypress(current_kpm);
}


void round_ui_show_splash(const char *text)
{
    if (!display_available) return;
    
    if (lvgl_port_lock(200)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        apply_dark_theme(scr);
        
        /* Create centered splash text */
        lv_obj_t *splash = lv_label_create(scr);
        lv_obj_set_style_text_font(splash, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(splash, COLOR_PRIMARY, 0);
        lv_obj_set_style_text_align(splash, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(splash, text);
        lv_obj_center(splash);
        
        /* Add decorative arc */
        lv_obj_t *arc = lv_arc_create(scr);
        lv_obj_set_size(arc, 200, 200);
        lv_obj_center(arc);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, COLOR_ARC_BG, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, COLOR_PRIMARY, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 100);
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        
        ui_initialized = false;
        ui_showing_splash = true;  /* Block layer updates during splash */
        
        lvgl_port_unlock();
    }
}

void round_ui_show_dfu(void)
{
    round_ui_show_splash("DFU Mode");
}

bool round_ui_is_initialized(void)
{
    return ui_initialized;
}

bool round_ui_is_sleeping(void)
{
    return ui_sleeping;
}
