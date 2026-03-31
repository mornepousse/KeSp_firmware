/* OLED I2C (SSD1306) backend implementation */
#include "display_backend.h"
#include "status_display.h"
#include "board.h"
#include "i2c_oled_display.h"
#include "hid_bluetooth_manager.h"
#include "hid_report.h"
#include "keyboard_config.h"
#include "keymap.h"
#include "tama_engine.h"
#include "tama_render.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "imgs.h"

LV_FONT_DECLARE(lv_font_montserrat_28);

#ifndef BT_BLINK_INTERVAL_MS
#define BT_BLINK_INTERVAL_MS    500
#endif
#ifndef MOUSE_INDICATOR_MS
#define MOUSE_INDICATOR_MS      200
#endif

/*
 * OLED layout D — "Card" (128×64, SSD1306)
 *
 *  Y=0..15   status card  (16px — rounded border box)
 *              ╭──────────────────────────────────────╮
 *              │  ⚡  ᯡ  1                        M   │
 *              ╰──────────────────────────────────────╯
 *
 *  Y=16..57  content (42px)
 *              left 94px: layer name (font_28 or font_14)
 *              right 32px: tama sprite inside a square box
 *                          ┌───────┐
 *                          │  🐣   │
 *                          │ 32×32 │
 *                          └───────┘
 *
 *  Y=59..62  KPM bar (4px)
 *
 * With tama OFF the full 128px are used for layer name and KPM bar.
 *
 * Layer name font selection:
 *   tama OFF  or  name ≤ 3 chars → font_28, centred in available width
 *   tama ON   and name ≥ 4 chars → font_14, centred in left zone
 */

/* Status bar (card: transparent bg, lit border — Layout D)
 * On this OLED: lv_color_black()=pixel ON (lit), lv_color_white()=pixel OFF (dark) */
#define OLED_STATUS_H           16
#define OLED_STATUS_RADIUS      4     /* rounded corners — visible on 128px */
#define OLED_ICON_PATH_X        4
#define OLED_ICON_BT_X          22
#define OLED_BT_SLOT_X          42
#define OLED_MOUSE_X            106   /* right of status card: 128 − 14 − 8 */
#define OLED_MOUSE_Y            1

/* Tama card (square border around the 32×32 sprite) */
#define OLED_TAMA_CARD_X        95
#define OLED_TAMA_CARD_Y        18    /* 2px gap after status bar border (y=15) */
#define OLED_TAMA_CARD_W        33
#define OLED_TAMA_CARD_H        39    /* y=18..56 */

/* Content area */
#define OLED_LAYER_X            0
#define OLED_LAYER_W_FULL       128
#define OLED_LAYER_W_TAMA       94
#define OLED_LAYER_Y_BIG        23    /* font_28 centred: 16+(42-28)/2 */
#define OLED_LAYER_Y_SMALL      30    /* font_14 centred: 16+(42-14)/2 */
#define OLED_LAYER_SHORT_LEN    3

/* KPM bar */
#define OLED_KPM_BAR_Y          59
#define OLED_KPM_BAR_H          4
#define OLED_KPM_BAR_W_TAMA     94
#define OLED_KPM_MAX            400

/* KPM rolling window */
#define OLED_KPM_WINDOW         60
#define OLED_KPM_SAMPLE_MS      1000

/* ── Module state ────────────────────────────────────────────────── */

static int         last_bt_state      = -1;
static int         last_path_state    = -1;
static int         last_pairing_state = -1;
static lv_obj_t   *status_card        = NULL;
static lv_obj_t   *tama_card          = NULL;
static lv_obj_t   *icon_bt            = NULL;
static lv_obj_t   *icon_path          = NULL;
static lv_obj_t   *indicator_mouse    = NULL;
static lv_obj_t   *label_layer_name   = NULL;
static lv_obj_t   *kpm_bar            = NULL;
static lv_obj_t   *bt_slot_label      = NULL;
static TickType_t  last_mouse_activity = 0;
static bool        bt_blink_visible   = true;
static TickType_t  bt_blink_last_tick = 0;
static bool        oled_initialized   = false;

static uint32_t    oled_kpm_count     = 0;
static uint32_t    oled_kpm_history[OLED_KPM_WINDOW];
static int         oled_kpm_idx       = 0;
static uint32_t    oled_current_kpm   = 0;
static TickType_t  oled_last_kpm_sample = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Helper: create a card-style lv_obj (rounded or square border, transparent bg).
   Must be called with lvgl_port_lock held. */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_opa(card, LV_OPA_0, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0); /* black = pixel ON = lit */
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/* Apply font/position to layer label based on tama state and name length.
   Must be called with lvgl_port_lock held. */
static void apply_layer_style(bool tama_on, const char *name)
{
    if (!label_layer_name) return;
    bool short_name = (strlen(name) <= OLED_LAYER_SHORT_LEN);

    if (!tama_on || short_name) {
        lv_obj_set_style_text_font(label_layer_name, &lv_font_montserrat_28, 0);
        lv_obj_set_pos(label_layer_name, OLED_LAYER_X, OLED_LAYER_Y_BIG);
        lv_obj_set_width(label_layer_name, tama_on ? OLED_LAYER_W_TAMA : OLED_LAYER_W_FULL);
    } else {
        lv_obj_set_style_text_font(label_layer_name, UI_FONT, 0);
        lv_obj_set_pos(label_layer_name, OLED_LAYER_X, OLED_LAYER_Y_SMALL);
        lv_obj_set_width(label_layer_name, OLED_LAYER_W_TAMA);
    }
}

/* ── UI construction ─────────────────────────────────────────────── */

static void oled_init_icons(void)
{
    if (!display_available) return;
    if (icon_bt != NULL || icon_path != NULL) return;

    lv_obj_t *scr = lv_scr_act();
    bool tama_on  = tama_engine_is_enabled();

    /* — Status bar: card with rounded lit border, transparent bg (Layout D) — */
    status_card = make_card(scr, 0, 0, BOARD_DISPLAY_WIDTH, OLED_STATUS_H, OLED_STATUS_RADIUS);

    /* — Icons: inherit lit text_color from screen theme (no recolor needed) — */
    icon_path = lv_img_create(scr);
    lv_obj_set_pos(icon_path, OLED_ICON_PATH_X, 0);

    icon_bt = lv_img_create(scr);
    lv_obj_set_pos(icon_bt, OLED_ICON_BT_X, 0);

    bt_slot_label = lv_label_create(scr);
    lv_obj_set_style_text_font(bt_slot_label, UI_FONT, 0);
    lv_label_set_text(bt_slot_label, "");
    lv_obj_set_pos(bt_slot_label, OLED_BT_SLOT_X, OLED_MOUSE_Y);

    indicator_mouse = lv_label_create(scr);
    lv_label_set_text(indicator_mouse, "M");
    lv_obj_set_style_text_font(indicator_mouse, UI_FONT, 0);
    lv_obj_set_pos(indicator_mouse, OLED_MOUSE_X, OLED_MOUSE_Y);
    lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);

    /* — Tama card (square border around tama sprite, only when active) — */
    if (tama_on) {
        tama_card = make_card(scr, OLED_TAMA_CARD_X, OLED_TAMA_CARD_Y,
                              OLED_TAMA_CARD_W, OLED_TAMA_CARD_H, 0);
    }

    /* — Layer name — */
    const char *name = default_layout_names[current_layout];
    label_layer_name = lv_label_create(scr);
    lv_label_set_text(label_layer_name, name);
    lv_label_set_long_mode(label_layer_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label_layer_name, LV_TEXT_ALIGN_CENTER, 0);
    apply_layer_style(tama_on, name);

    /* — KPM bar (transparent bg, white fill) — */
    int kpm_w = tama_on ? OLED_KPM_BAR_W_TAMA : BOARD_DISPLAY_WIDTH;
    kpm_bar = lv_bar_create(scr);
    lv_obj_set_size(kpm_bar, kpm_w, OLED_KPM_BAR_H);
    lv_obj_set_pos(kpm_bar, 0, OLED_KPM_BAR_Y);
    lv_bar_set_range(kpm_bar, 0, OLED_KPM_MAX);
    lv_bar_set_value(kpm_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(kpm_bar, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(kpm_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(kpm_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(kpm_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kpm_bar, lv_color_black(), LV_PART_INDICATOR); /* pixel ON = lit */
    lv_obj_set_style_bg_opa(kpm_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(kpm_bar, 0, LV_PART_INDICATOR);
}

static void oled_prepare_ui(bool clear_screen)
{
    if (!display_available) return;
    if (!lvgl_port_lock(200)) return;

    if (clear_screen) {
        display_clear_screen();
        status_card      = NULL;
        tama_card        = NULL;
        icon_bt          = NULL;
        icon_path        = NULL;
        indicator_mouse  = NULL;
        label_layer_name  = NULL;
        kpm_bar           = NULL;
        bt_slot_label     = NULL;
        last_bt_state      = -1;
        last_path_state    = -1;
        last_pairing_state = -1;
        bt_blink_visible = true;
        bt_blink_last_tick = xTaskGetTickCount();
        oled_kpm_count   = 0;
        oled_kpm_idx     = 0;
        oled_current_kpm = 0;
        memset(oled_kpm_history, 0, sizeof(oled_kpm_history));
        oled_last_kpm_sample = xTaskGetTickCount();
    }

    oled_init_icons();
    oled_initialized = true;
    lvgl_port_unlock();
}

static void oled_update_connection_icons(bool force)
{
    if (!display_available) return;

    int bt_state;
    if (!hid_bluetooth_is_initialized())      bt_state = 0;
    else if (hid_bluetooth_is_connected())    bt_state = 1;
    else                                       bt_state = 2;

    int path_state = (keyboard_get_usb_bl_state() == 0) ? 0 : 1;
    int pairing_state = hid_bluetooth_is_pairing() ? 1 : 0;

    bool blink_toggled = false;
    if (bt_state == 2 && !force) {
        TickType_t now = xTaskGetTickCount();
        if ((now - bt_blink_last_tick) >= pdMS_TO_TICKS(BT_BLINK_INTERVAL_MS)) {
            bt_blink_visible   = !bt_blink_visible;
            bt_blink_last_tick = now;
            blink_toggled      = true;
        }
    }

    if (!force && bt_state == last_bt_state && path_state == last_path_state
        && pairing_state == last_pairing_state && !blink_toggled)
        return;

    if (!lvgl_port_lock(50)) return;
    oled_init_icons();
    if (!icon_path) { lvgl_port_unlock(); return; }

    /* Connection path icon */
    if (path_state == 0) lv_img_set_src(icon_path, &flash);
    else                  lv_img_set_src(icon_path, &bluetooth_16px);

    /* BT status icon */
    if (bt_state == 0) {
        bt_blink_visible = false;
        if (icon_bt) lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
    } else if (bt_state == 1) {
        bt_blink_visible = true;
        if (icon_bt) { lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN); lv_img_set_src(icon_bt, &wifi); }
    } else {
        if (force) { bt_blink_visible = true; bt_blink_last_tick = xTaskGetTickCount(); }
        if (icon_bt) {
            if (bt_blink_visible) { lv_obj_clear_flag(icon_bt, LV_OBJ_FLAG_HIDDEN); lv_img_set_src(icon_bt, &wifi); }
            else                    lv_obj_add_flag(icon_bt, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* BT slot: always show slot number when BT on, "P" only in true pairing mode.
     * The blinking icon_bt already indicates "not connected / searching". */
    if (bt_slot_label) {
        if (bt_state > 0 && hid_bluetooth_is_pairing())
            lv_label_set_text(bt_slot_label, "P");
        else if (bt_state > 0)
            lv_label_set_text_fmt(bt_slot_label, "%d", bt_get_active_slot() + 1);
        else
            lv_label_set_text(bt_slot_label, "");
    }

    last_bt_state      = bt_state;
    last_path_state    = path_state;
    last_pairing_state = pairing_state;
    lvgl_port_unlock();
}

/* ── Backend interface ───────────────────────────────────────────── */

static bool oled_init(void)
{
    display_hw_config_t cfg = {
        .bus_type       = BOARD_DISPLAY_BUS,
        .width          = BOARD_DISPLAY_WIDTH,
        .height         = BOARD_DISPLAY_HEIGHT,
        .pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
        .reset_pin      = BOARD_DISPLAY_RESET,
        .i2c = {
            .host                   = BOARD_DISPLAY_I2C_HOST,
            .sda                    = BOARD_DISPLAY_I2C_SDA,
            .scl                    = BOARD_DISPLAY_I2C_SCL,
            .address                = BOARD_DISPLAY_I2C_ADDR,
            .enable_internal_pullups = BOARD_DISPLAY_I2C_PULLUPS,
        },
    };
    display_set_hw_config(&cfg);
    init_display();
    return display_available;
}

static void oled_update_layer(void)
{
    if (!oled_initialized) oled_prepare_ui(true);
    else                    oled_prepare_ui(false);

    if (label_layer_name && lvgl_port_lock(50)) {
        const char *name = default_layout_names[current_layout];
        lv_label_set_text(label_layer_name, name);
        apply_layer_style(tama_engine_is_enabled(), name);
        lvgl_port_unlock();
    }
}

static void oled_update(void)
{
    oled_update_connection_icons(false);

    /* KPM sample — every 1 second, outside LVGL lock */
    TickType_t now = xTaskGetTickCount();
    if ((now - oled_last_kpm_sample) >= pdMS_TO_TICKS(OLED_KPM_SAMPLE_MS)) {
        oled_kpm_history[oled_kpm_idx] = oled_kpm_count;
        oled_kpm_idx = (oled_kpm_idx + 1) % OLED_KPM_WINDOW;
        oled_kpm_count = 0;
        oled_last_kpm_sample = now;
        uint32_t total = 0;
        for (int i = 0; i < OLED_KPM_WINDOW; i++) total += oled_kpm_history[i];
        oled_current_kpm = total;
    }

    if (!lvgl_port_lock(50)) return;

    /* Mouse indicator */
    if (indicator_mouse) {
        if ((xTaskGetTickCount() - last_mouse_activity) < pdMS_TO_TICKS(MOUSE_INDICATOR_MS))
            lv_obj_clear_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(indicator_mouse, LV_OBJ_FLAG_HIDDEN);
    }

    /* KPM bar */
    if (kpm_bar) {
        uint32_t v = oled_current_kpm > OLED_KPM_MAX ? OLED_KPM_MAX : oled_current_kpm;
        lv_bar_set_value(kpm_bar, (int32_t)v, LV_ANIM_OFF);
    }

    /* Tama — must be called with lock held */
    if (tama_engine_is_enabled())
        tama_render_update(tama_engine_get_state(), tama_engine_get_stats(), tama_engine_get_critter());

    lvgl_port_unlock();
}

static void oled_refresh_all(void)
{
    oled_prepare_ui(true);
    oled_update_layer();
    oled_update_connection_icons(true);

    if (tama_engine_is_enabled() && lvgl_port_lock(50)) {
        lv_obj_t *scr = lv_scr_act();
        tama_render_create(scr, BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
        lvgl_port_unlock();
    }
}

static void oled_sleep(void)
{
    if (lvgl_port_lock(100)) {
        display_clear_screen();
        status_card      = NULL;
        tama_card        = NULL;
        icon_bt          = NULL;
        icon_path        = NULL;
        indicator_mouse  = NULL;
        label_layer_name  = NULL;
        kpm_bar           = NULL;
        bt_slot_label     = NULL;
        last_bt_state      = -1;
        last_path_state    = -1;
        last_pairing_state = -1;
        tama_render_destroy();
        oled_initialized = false;
        lvgl_port_unlock();
    }
}

static void oled_wake(void)
{
    request_wake_request = true;
}

static void oled_notify_mouse(void)
{
    last_mouse_activity = xTaskGetTickCount();
}

static void oled_notify_keypress(void)
{
    oled_kpm_count++;
    if (tama_engine_is_enabled())
        tama_engine_keypress(oled_current_kpm);
}

static void oled_show_dfu(void)
{
    display_clear_screen();
    if (lvgl_port_lock(0)) {
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_label_set_text(label, "DFU");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lvgl_port_unlock();
    }
}

const display_backend_t oled_display_backend = {
    .init            = oled_init,
    .update_layer    = oled_update_layer,
    .update          = oled_update,
    .refresh_all     = oled_refresh_all,
    .sleep           = oled_sleep,
    .wake            = oled_wake,
    .notify_mouse    = oled_notify_mouse,
    .notify_keypress = oled_notify_keypress,
    .show_dfu        = oled_show_dfu,
};
