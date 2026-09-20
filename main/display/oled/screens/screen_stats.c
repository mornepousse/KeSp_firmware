/* screen_stats.c — STATS screen: KPM / WPM / sparkline / total keystrokes.
 *
 * 128×64 SSD1306 mono layout (lv_color_black() = lit pixel):
 *
 *   y=0..13  : top — "KPM NNN" (left) + "NN wpm" (right)
 *   y=15..50 : sparkline — SS_N_BARS thin bars (lit background, grows upward)
 *   y=52..63 : bottom — total keystrokes "1.2M keys"
 *
 * build/update/destroy contract:
 *   build(parent)  — creates the objects; parent = lv_scr_act(), lock already held.
 *   update()       — refreshes the dynamic values, no object creation.
 *   destroy()      — lv_obj_del of each non-NULL object (labels + the N bars), NULL.
 *
 * The LVGL lock is held by the caller (oled_screens_tick) — no auto-lock here.
 */

#include "oled_screen.h"
#include "lvgl.h"
#include "board.h"
#include "oled_kpm.h"
#include "oled_stats.h"
#include "key_stats.h"
#include <stdio.h>
#include <stdint.h>

/* ── Layout constants ─────────────────────────────────────────────── */

#define SS_N_BARS           20     /* number of sparkline bars                 */
#define SS_BAR_W             5     /* width of a bar (px)                      */
#define SS_BAR_GAP           1     /* gap between bars (px)                    */
#define SS_SPARK_X           4     /* start x of the first bar                 */
/* Total sparkline width = SS_N_BARS*SS_BAR_W + (SS_N_BARS-1)*SS_BAR_GAP
 *   = 20*5 + 19*1 = 119 px  →  end x = 4+119 = 123 < 128  ✓               */

#define SS_TOP_H            14     /* top zone height (px)                     */
#define SS_SPARK_Y          15     /* start y of the sparkline zone             */
#define SS_SPARK_H          31     /* height of the sparkline zone (px)        */
#define SS_SPARK_BOTTOM     (SS_SPARK_Y + SS_SPARK_H) /* = 46, baseline       */
#define SS_PX_PER_UNIT       4     /* px per bar unit (0..7 → 0..28 px)        */
#define SS_BOTTOM_Y         48     /* y of the total: 48+14=62 < 64, no clip   */

/* ── Static pointers to the LVGL objects ────────────────────────────── */

static lv_obj_t *s_kpm_label   = NULL;
static lv_obj_t *s_wpm_label   = NULL;
static lv_obj_t *s_total_label = NULL;
static lv_obj_t *s_bars[SS_N_BARS];  /* sparkline bars */

/* ── Helper: formats the total in K/M without floating point ─────────── */

static void format_total(char *buf, size_t n, uint32_t v)
{
    if (v >= 1000000u) {
        uint32_t m = v / 1000000u;
        uint32_t d = (v % 1000000u) / 100000u;  /* first decimal digit */
        snprintf(buf, n, "%lu.%luM keys", (unsigned long)m, (unsigned long)d);
    } else if (v >= 1000u) {
        snprintf(buf, n, "%luK keys", (unsigned long)(v / 1000u));
    } else {
        snprintf(buf, n, "%lu keys", (unsigned long)v);
    }
}

/* ── build ────────────────────────────────────────────────────────────── */

static void build(lv_obj_t *parent)
{
    /* Initialize the array before filling it (partial-destroy safety) */
    for (int i = 0; i < SS_N_BARS; i++) s_bars[i] = NULL;

    /* Top-left zone: KPM value */
    s_kpm_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_kpm_label, UI_FONT, 0);
    lv_label_set_text(s_kpm_label, "KPM --");
    lv_obj_set_pos(s_kpm_label, 2, 0);

    /* Top-right zone: WPM value (right-aligned) */
    s_wpm_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_wpm_label, UI_FONT, 0);
    lv_label_set_text(s_wpm_label, "-- wpm");
    /* Positioned at BOARD_DISPLAY_WIDTH - 58 to leave room for "400 wpm" */
    lv_obj_set_pos(s_wpm_label, BOARD_DISPLAY_WIDTH - 58, 0);

    /* Sparkline: SS_N_BARS thin rectangles, lit background, growing upward */
    for (int i = 0; i < SS_N_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        /* Initial size: 1 px tall (no creation in update) */
        lv_obj_set_size(bar, SS_BAR_W, 1);
        int bx = SS_SPARK_X + i * (SS_BAR_W + SS_BAR_GAP);
        lv_obj_set_pos(bar, bx, SS_SPARK_BOTTOM - 1);
        s_bars[i] = bar;
    }

    /* Bottom zone: total keystrokes */
    s_total_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_total_label, UI_FONT, 0);
    lv_label_set_text(s_total_label, "0 keys");
    lv_obj_set_pos(s_total_label, 2, SS_BOTTOM_Y);
}

/* ── update ───────────────────────────────────────────────────────────── */

static void update(void)
{
    uint32_t kpm = oled_kpm_value();
    uint32_t wpm = oled_wpm_from_kpm(kpm);

    /* 1. KPM / WPM labels ─────────────────────────────────────────────── */
    if (s_kpm_label)
        lv_label_set_text_fmt(s_kpm_label, "%lukpm  %luwpm",
                              (unsigned long)kpm, (unsigned long)wpm);
    if (s_wpm_label)
        lv_label_set_text(s_wpm_label, "");   /* merged into s_kpm_label */

    /* 2. Sparkline ────────────────────────────────────────────────────── */
    {
        const uint32_t *hist = oled_kpm_history();
        /* Auto-scale on the window's peak (samples = keystrokes/second
         * ~3..25, not the cumulative KPM); floor of 8 to avoid amplifying noise. */
        uint32_t hmax = 8;
        for (int i = 0; i < (int)OLED_KPM_WINDOW; i++)
            if (hist[i] > hmax) hmax = hist[i];
        uint8_t heights[SS_N_BARS];
        oled_sparkline_bars(hist, (int)OLED_KPM_WINDOW, hmax,
                            heights, SS_N_BARS);
        for (int i = 0; i < SS_N_BARS; i++) {
            if (!s_bars[i]) continue;
            int bar_h = (int)heights[i] * SS_PX_PER_UNIT;
            if (bar_h < 1) bar_h = 1;  /* 1 px floor (always visible) */
            int bx = SS_SPARK_X + i * (SS_BAR_W + SS_BAR_GAP);
            lv_obj_set_size(s_bars[i], SS_BAR_W, bar_h);
            lv_obj_set_pos(s_bars[i],  bx, SS_SPARK_BOTTOM - bar_h);
        }
    }

    /* 3. Total keystrokes ────────────────────────────────────────────────── */
    if (s_total_label) {
        char buf[24];
        format_total(buf, sizeof(buf), key_stats_total);
        lv_label_set_text(s_total_label, buf);
    }
}

/* ── destroy ──────────────────────────────────────────────────────────── */

static void destroy(void)
{
    if (s_kpm_label   && lv_obj_is_valid(s_kpm_label))   lv_obj_del(s_kpm_label);
    s_kpm_label = NULL;
    if (s_wpm_label   && lv_obj_is_valid(s_wpm_label))   lv_obj_del(s_wpm_label);
    s_wpm_label = NULL;
    if (s_total_label && lv_obj_is_valid(s_total_label)) lv_obj_del(s_total_label);
    s_total_label = NULL;
    for (int i = 0; i < SS_N_BARS; i++) {
        if (s_bars[i] && lv_obj_is_valid(s_bars[i])) lv_obj_del(s_bars[i]);
        s_bars[i] = NULL;
    }
}

const oled_screen_t screen_stats = { build, update, destroy };
