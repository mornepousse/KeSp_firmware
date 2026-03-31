/* Round display UI — GC9A01 240×240 */
#include "round_ui.h"
#include "tama_engine.h"
#include "tama_render.h"
#include "status_display.h"
#include "keyboard_config.h"
#include "hid_bluetooth_manager.h"
#include "hid_report.h"
#include "keymap.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ROUND_UI"

#ifndef BT_BLINK_INTERVAL_MS
#define BT_BLINK_INTERVAL_MS    500
#endif
#ifndef MOUSE_INDICATOR_MS
#define MOUSE_INDICATOR_MS      200
#endif

/* Display */
#define DISP_SIZE BOARD_DISPLAY_WIDTH

/* Colors */
#define COLOR_BG        lv_color_hex(0x0D1117)
#define COLOR_PRIMARY   lv_color_hex(0x58A6FF)
#define COLOR_SECONDARY lv_color_hex(0x8B949E)
#define COLOR_SUCCESS   lv_color_hex(0x3FB950)
#define COLOR_WARNING   lv_color_hex(0xF0883E)
#define COLOR_TEXT      lv_color_hex(0xE6EDF3)
#define COLOR_ARC_BG    lv_color_hex(0x21262D)

/*
 * Layout — 240×240 round, r=120
 *
 *  Outer arc  (230×230, 270°, 4px) — KPM progress ring
 *
 *  Status bar  y≈28:
 *    conn_icon  TOP_MID(-42, 21) ← path: USB or BT/WiFi
 *    status_icon TOP_MID(+22, 21)  ← BT status
 *    bt_slot    TOP_MID(+38, 21)   ← "1"/"2"/"3"
 *    (conn moves to -6 when BT is off)
 *
 *  Tama OFF:
 *    layer_label CENTER(0, 0)    font_28 — dominant
 *    kpm_label   BOTTOM_MID(0, -48) font_14
 *
 *  Tama ON:
 *    layer_label TOP_MID(0, 46)  font_14 — secondary, above tama
 *    tama sprite CENTER(0, -20)  64×64px (y≈68..176)
 *    kpm_label   BOTTOM_MID(0, -56) font_14 — below tama
 *
 *  Mouse indicator  BOTTOM_MID(-55, -48)  — 7-o'clock, hidden most of time
 */

/* Font declarations */
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_14);

/* UI objects */
static lv_obj_t *outer_arc       = NULL;
static lv_obj_t *layer_label     = NULL;
static lv_obj_t *status_icon     = NULL;
static lv_obj_t *conn_icon       = NULL;
static lv_obj_t *bt_slot_label   = NULL;
static lv_obj_t *mouse_indicator = NULL;
static lv_obj_t *kpm_label       = NULL;

/* State */
static bool      ui_initialized    = false;
static bool      ui_sleeping       = false;
static bool      ui_showing_splash = false;
static bool      tama_was_enabled  = false;
static int       last_bt_state     = -1;
static int       last_path_state   = -1;
static TickType_t last_mouse_activity = 0;
static TickType_t bt_blink_last_tick  = 0;
static bool      bt_blink_visible  = true;

/* KPM */
#ifndef KPM_WINDOW_SIZE
#define KPM_WINDOW_SIZE         60
#endif
#ifndef KPM_SAMPLE_INTERVAL_MS
#define KPM_SAMPLE_INTERVAL_MS  1000
#endif
#ifndef KPM_MAX_DISPLAY
#define KPM_MAX_DISPLAY         400
#endif

static uint32_t  keypress_count   = 0;
static uint32_t  kpm_history[KPM_WINDOW_SIZE];
static int       kpm_history_index = 0;
uint32_t         current_kpm      = 0;
static TickType_t last_kpm_sample = 0;

/* Forward declarations */
static void create_main_ui(void);
static void update_connection_status(bool force);

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Pick arc/label color based on KPM */
static lv_color_t kpm_color(uint32_t kpm)
{
    if (kpm > 300)      return COLOR_SUCCESS;
    else if (kpm > 150) return COLOR_PRIMARY;
    else if (kpm > 30)  return COLOR_WARNING;
    else                return COLOR_SECONDARY;
}

/* Apply layer label style/position for current tama state.
   Must be called with lvgl_port_lock held. */
static void apply_round_layer_style(bool tama_on)
{
    if (!layer_label || !kpm_label) return;

    if (tama_on) {
        lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(layer_label, COLOR_SECONDARY, 0);
        lv_obj_set_width(layer_label, 130);
        lv_label_set_long_mode(layer_label, LV_LABEL_LONG_DOT);
        lv_obj_align(layer_label, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_align(kpm_label, LV_ALIGN_BOTTOM_MID, 0, -56);
    } else {
        lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(layer_label, COLOR_TEXT, 0);
        lv_obj_set_width(layer_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(layer_label, LV_LABEL_LONG_CLIP);
        lv_obj_align(layer_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(kpm_label, LV_ALIGN_BOTTOM_MID, 0, -48);
    }
}

/* ── UI construction ─────────────────────────────────────────────── */

static void create_outer_arc(lv_obj_t *parent)
{
    outer_arc = lv_arc_create(parent);
    lv_obj_set_size(outer_arc, 230, 230);
    lv_obj_center(outer_arc);

    lv_obj_set_style_arc_width(outer_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(outer_arc, COLOR_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(outer_arc, true, LV_PART_MAIN);

    lv_obj_set_style_arc_width(outer_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(outer_arc, COLOR_SECONDARY, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(outer_arc, true, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(outer_arc, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_arc_set_range(outer_arc, 0, 100);
    lv_arc_set_value(outer_arc, 2);
    lv_arc_set_bg_angles(outer_arc, 135, 45);
    lv_arc_set_rotation(outer_arc, 0);
    lv_obj_clear_flag(outer_arc, LV_OBJ_FLAG_CLICKABLE);
}

static void create_status_icons(lv_obj_t *parent)
{
    /* Connection path — left side */
    conn_icon = lv_label_create(parent);
    lv_obj_set_style_text_font(conn_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(conn_icon, COLOR_PRIMARY, 0);
    lv_label_set_text(conn_icon, LV_SYMBOL_USB);
    lv_obj_align(conn_icon, LV_ALIGN_TOP_MID, -6, 21);  /* centered until BT is known */

    /* BT status — right side */
    status_icon = lv_label_create(parent);
    lv_obj_set_style_text_font(status_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_icon, COLOR_SUCCESS, 0);
    lv_label_set_text(status_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(status_icon, LV_ALIGN_TOP_MID, +22, 21);
    lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN);

    /* BT slot digit — next to BT icon */
    bt_slot_label = lv_label_create(parent);
    lv_obj_set_style_text_font(bt_slot_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bt_slot_label, COLOR_SECONDARY, 0);
    lv_label_set_text(bt_slot_label, "");
    lv_obj_align(bt_slot_label, LV_ALIGN_TOP_MID, +38, 21);
}

static void create_layer_display(lv_obj_t *parent)
{
    layer_label = lv_label_create(parent);
    lv_obj_set_style_text_align(layer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(layer_label, default_layout_names[current_layout]);
    /* Style applied by apply_round_layer_style() after creation */
}

static void create_kpm_label(lv_obj_t *parent)
{
    kpm_label = lv_label_create(parent);
    lv_obj_set_style_text_font(kpm_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(kpm_label, COLOR_SECONDARY, 0);
    lv_obj_set_style_text_align(kpm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(kpm_label, "0 KPM");
    /* Position set by apply_round_layer_style() */
}

static void create_mouse_indicator(lv_obj_t *parent)
{
    mouse_indicator = lv_label_create(parent);
    lv_obj_set_style_text_font(mouse_indicator, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mouse_indicator, COLOR_WARNING, 0);
    lv_label_set_text(mouse_indicator, LV_SYMBOL_EYE_OPEN);
    lv_obj_align(mouse_indicator, LV_ALIGN_BOTTOM_MID, -55, -48);
    lv_obj_add_flag(mouse_indicator, LV_OBJ_FLAG_HIDDEN);
}

static void create_main_ui(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    create_outer_arc(scr);
    create_status_icons(scr);
    create_layer_display(scr);
    create_kpm_label(scr);
    create_mouse_indicator(scr);

    /* Apply initial layout based on tama state */
    tama_was_enabled = tama_engine_is_enabled();
    apply_round_layer_style(tama_was_enabled);

    memset(kpm_history, 0, sizeof(kpm_history));
    kpm_history_index = 0;
    keypress_count    = 0;
    current_kpm       = 0;
    last_kpm_sample   = xTaskGetTickCount();
}

/* ── Connection status update ────────────────────────────────────── */

static void update_connection_status(bool force)
{
    if (!display_available || ui_sleeping) return;

    int bt_state;
    if (!hid_bluetooth_is_initialized())    bt_state = 0;
    else if (hid_bluetooth_is_connected())  bt_state = 1;
    else                                     bt_state = 2;

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;

    if (!force && bt_state == last_bt_state && path_state == last_path_state && bt_state != 2)
        return;

    if (!lvgl_port_lock(50)) return;

    /* Connection path icon + dynamic x position */
    if (conn_icon) {
        if (path_state == 0) {
            lv_label_set_text(conn_icon, LV_SYMBOL_USB);
            lv_obj_set_style_text_color(conn_icon, COLOR_PRIMARY, 0);
        } else {
            lv_label_set_text(conn_icon, LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(conn_icon, COLOR_SUCCESS, 0);
        }
        /* Push left when BT icons are visible, centre when alone */
        int conn_x = (bt_state > 0) ? -42 : -6;
        lv_obj_align(conn_icon, LV_ALIGN_TOP_MID, conn_x, 21);
    }

    /* BT status icon */
    if (status_icon) {
        if (bt_state == 0) {
            lv_obj_add_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
        } else if (bt_state == 1) {
            lv_obj_clear_flag(status_icon, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(status_icon, LV_SYMBOL_BLUETOOTH);
            lv_obj_set_style_text_color(status_icon, COLOR_SUCCESS, 0);
        } else {
            /* Advertising — blink */
            TickType_t now = xTaskGetTickCount();
            if (force) { bt_blink_visible = true; bt_blink_last_tick = now; }
            else if ((now - bt_blink_last_tick) >= pdMS_TO_TICKS(BT_BLINK_INTERVAL_MS)) {
                bt_blink_visible  = !bt_blink_visible;
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

    /* BT slot: always show slot number when BT on, "P" only in true pairing mode */
    if (bt_slot_label) {
        if (bt_state > 0 && hid_bluetooth_is_pairing()) {
            lv_label_set_text(bt_slot_label, "P");
            lv_obj_clear_flag(bt_slot_label, LV_OBJ_FLAG_HIDDEN);
        } else if (bt_state > 0) {
            lv_label_set_text_fmt(bt_slot_label, "%d", bt_get_active_slot() + 1);
            lv_obj_clear_flag(bt_slot_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bt_slot_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    last_bt_state   = bt_state;
    last_path_state = path_state;
    lvgl_port_unlock();
}

/* ── Public API ──────────────────────────────────────────────────── */

void round_ui_init(void)
{
    if (!display_available) {
        ESP_LOGW(TAG, "Display not available, skipping UI init");
        return;
    }

    if (lvgl_port_lock(200)) {
        create_main_ui();
        tama_render_create(lv_scr_act(), BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
        ui_initialized    = true;
        ui_sleeping       = false;
        ui_showing_splash = false;
        lvgl_port_unlock();
    }

    update_connection_status(true);
}

void round_ui_update_layer(void)
{
    if (!display_available || ui_sleeping || ui_showing_splash) return;
    if (!ui_initialized) { round_ui_init(); return; }

    if (layer_label && lvgl_port_lock(100)) {
        lv_label_set_text(layer_label, default_layout_names[current_layout]);
        lvgl_port_unlock();
    }
}

void round_ui_update(void)
{
    if (!display_available || ui_sleeping) return;

    update_connection_status(false);

    /* KPM sampling */
    TickType_t now = xTaskGetTickCount();
    if ((now - last_kpm_sample) >= pdMS_TO_TICKS(KPM_SAMPLE_INTERVAL_MS)) {
        kpm_history[kpm_history_index] = keypress_count;
        kpm_history_index = (kpm_history_index + 1) % KPM_WINDOW_SIZE;
        keypress_count = 0;
        last_kpm_sample = now;
        uint32_t total = 0;
        for (int i = 0; i < KPM_WINDOW_SIZE; i++) total += kpm_history[i];
        current_kpm = total;
    }

    if (!lvgl_port_lock(50)) return;

    /* Detect tama state change → relayout */
    bool tama_on = tama_engine_is_enabled();
    if (tama_on != tama_was_enabled) {
        tama_was_enabled = tama_on;
        apply_round_layer_style(tama_on);
    }

    /* Arc */
    if (outer_arc) {
        int32_t v = (int32_t)((current_kpm * 100) / KPM_MAX_DISPLAY);
        if (v > 100) v = 100;
        if (v < 2)   v = 2;
        lv_arc_set_value(outer_arc, v);
        lv_obj_set_style_arc_color(outer_arc, kpm_color(current_kpm), LV_PART_INDICATOR);
    }

    /* KPM label */
    if (kpm_label) {
        lv_label_set_text_fmt(kpm_label, "%lu KPM", (unsigned long)current_kpm);
        lv_obj_set_style_text_color(kpm_label, kpm_color(current_kpm), 0);
    }

    /* Mouse indicator */
    if (mouse_indicator) {
        if ((xTaskGetTickCount() - last_mouse_activity) < pdMS_TO_TICKS(MOUSE_INDICATOR_MS))
            lv_obj_clear_flag(mouse_indicator, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(mouse_indicator, LV_OBJ_FLAG_HIDDEN);
    }

    /* Tama */
    if (tama_on)
        tama_render_update(tama_engine_get_state(), tama_engine_get_stats(), tama_engine_get_critter());

    lvgl_port_unlock();
}

void round_ui_sleep(void)
{
    if (!display_available || ui_sleeping) return;

    if (lvgl_port_lock(100)) {
        lv_obj_clean(lv_scr_act());
        outer_arc       = NULL;
        layer_label     = NULL;
        status_icon     = NULL;
        conn_icon       = NULL;
        bt_slot_label   = NULL;
        mouse_indicator = NULL;
        kpm_label       = NULL;
        tama_render_destroy();
        ui_sleeping    = true;
        ui_initialized = false;
        last_bt_state  = -1;
        last_path_state = -1;
        lvgl_port_unlock();
    }
}

void round_ui_wake(void)
{
    if (!display_available || !ui_sleeping) return;
    ui_sleeping = false;
    round_ui_init();
    round_ui_update_layer();
}

void round_ui_refresh_all(void)
{
    if (!display_available) return;

    ui_sleeping       = false;
    ui_showing_splash = false;

    if (lvgl_port_lock(200)) {
        lv_obj_clean(lv_scr_act());
        outer_arc       = NULL;
        layer_label     = NULL;
        status_icon     = NULL;
        conn_icon       = NULL;
        bt_slot_label   = NULL;
        mouse_indicator = NULL;
        kpm_label       = NULL;
        tama_render_destroy();
        create_main_ui();
        tama_render_create(lv_scr_act(), BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
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
        lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

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

        lv_obj_t *splash = lv_label_create(scr);
        lv_obj_set_style_text_font(splash, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(splash, COLOR_PRIMARY, 0);
        lv_obj_set_style_text_align(splash, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(splash, text);
        lv_obj_center(splash);

        ui_initialized    = false;
        ui_showing_splash = true;
        lvgl_port_unlock();
    }
}

void round_ui_show_dfu(void)
{
    round_ui_show_splash("DFU Mode");
}

bool round_ui_is_initialized(void) { return ui_initialized; }
bool round_ui_is_sleeping(void)    { return ui_sleeping; }
