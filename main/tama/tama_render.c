/* Tamagotchi LVGL renderer */
#include "tama_render.h"
#include "tama_sprites.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TAMA_RENDER";

/* ── UI elements ─────────────────────────────────────────────────── */

static lv_obj_t *container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *bar_hunger = NULL;
static lv_obj_t *bar_happy = NULL;
static lv_obj_t *bar_energy = NULL;
static lv_obj_t *label_level = NULL;

/* Animation state */
static bool anim_frame = false;    /* toggles between main/idle */
static uint32_t last_anim_ms = 0;
static int8_t bounce_offset = 0;   /* vertical bounce for idle breathing */
static uint8_t bounce_dir = 0;     /* 0=down, 1=up */

#define ANIM_FRAME_MS   600   /* frame toggle speed */
#define BOUNCE_STEP_MS  150   /* breathing bounce speed */
#define BOUNCE_RANGE    2     /* pixels of bounce */

/* Canvas buffer for round display (true-color) */
static lv_color_t canvas_buf[32 * 32];

/* Mono image buffer for OLED (1-bit, avoids canvas artifacts) */
static uint8_t mono_buf[4 + 32 * 4]; /* LVGL header (4 bytes) + 32 rows × 4 bytes */
static lv_img_dsc_t mono_img_dsc;

static uint16_t scr_w = 0, scr_h = 0;
static uint8_t scale = 1; /* pixel upscale factor */
static bool created = false;

/* ── Sprite drawing ──────────────────────────────────────────────── */

/* Draw a 32x32 mono bitmap onto the LVGL canvas, upscaled by `scale` */
static void draw_sprite(const uint8_t *bitmap, lv_color_t fg, lv_color_t bg)
{
    if (!canvas || !bitmap) return;

    uint16_t size = TAMA_SPRITE_W * scale;
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);

    for (int y = 0; y < TAMA_SPRITE_H; y++) {
        for (int x = 0; x < TAMA_SPRITE_W; x++) {
            int byte_idx = y * 4 + (x / 8);
            int bit_idx = 7 - (x % 8);
            bool pixel = (bitmap[byte_idx] >> bit_idx) & 1;

            if (pixel) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x * scale + sx;
                        int py = y * scale + sy;
                        if (px < size && py < size)
                            lv_canvas_set_px_color(canvas, px, py, fg);
                    }
                }
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void tama_render_create(lv_obj_t *parent, uint16_t screen_w, uint16_t screen_h)
{
    if (created) return;

    scr_w = screen_w;
    scr_h = screen_h;

    /* Always render at native 32x32 — use LVGL zoom for upscale */
    scale = 1;

    /* Sprite canvas — 32x32, zoomed by LVGL for round display */
    if (screen_w >= 240) {
        /* Round: true-color canvas with zoom */
        canvas = lv_canvas_create(parent);
        lv_canvas_set_buffer(canvas, canvas_buf, TAMA_SPRITE_W, TAMA_SPRITE_H, LV_IMG_CF_TRUE_COLOR);
        lv_img_set_zoom(canvas, 512);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, -20);
    } else {
        /* OLED mono: use lv_img with 1-bit buffer (no canvas artifacts) */
        mono_img_dsc.header.always_zero = 0;
        mono_img_dsc.header.w = TAMA_SPRITE_W;
        mono_img_dsc.header.h = TAMA_SPRITE_H;
        mono_img_dsc.header.cf = LV_IMG_CF_ALPHA_1BIT;
        mono_img_dsc.data_size = TAMA_SPRITE_BYTES;
        mono_img_dsc.data = mono_buf;
        memset(mono_buf, 0, sizeof(mono_buf));

        container = lv_img_create(parent);
        lv_img_set_src(container, &mono_img_dsc);
        lv_obj_set_pos(container, 96, 20); /* centered in content zone, safe from status bar */
    }

    /* Stat bars — below sprite (round) or left of sprite (OLED) */
    int bar_w = (screen_w >= 240) ? 70 : 30;
    int bar_h = (screen_w >= 240) ? 5 : 3;
    int bar_y_start = (screen_w >= 240) ? 20 : 0;

    if (screen_w >= 240) {
        /* Round: bars centered below sprite */
        bar_hunger = lv_bar_create(parent);
        lv_obj_set_size(bar_hunger, bar_w, bar_h);
        lv_bar_set_range(bar_hunger, 0, TAMA2_STAT_MAX);
        lv_obj_set_style_bg_color(bar_hunger, lv_color_hex(0x222222), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_hunger, lv_color_hex(0xFF8800), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_hunger, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(bar_hunger, 2, LV_PART_INDICATOR);
        lv_obj_align(bar_hunger, LV_ALIGN_CENTER, 0, bar_y_start);

        bar_happy = lv_bar_create(parent);
        lv_obj_set_size(bar_happy, bar_w, bar_h);
        lv_bar_set_range(bar_happy, 0, TAMA2_STAT_MAX);
        lv_obj_set_style_bg_color(bar_happy, lv_color_hex(0x222222), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_happy, lv_color_hex(0x00CC00), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_happy, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(bar_happy, 2, LV_PART_INDICATOR);
        lv_obj_align(bar_happy, LV_ALIGN_CENTER, 0, bar_y_start + bar_h + 3);

        bar_energy = lv_bar_create(parent);
        lv_obj_set_size(bar_energy, bar_w, bar_h);
        lv_bar_set_range(bar_energy, 0, TAMA2_STAT_MAX);
        lv_obj_set_style_bg_color(bar_energy, lv_color_hex(0x222222), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_energy, lv_color_hex(0x0088FF), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_energy, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(bar_energy, 2, LV_PART_INDICATOR);
        lv_obj_align(bar_energy, LV_ALIGN_CENTER, 0, bar_y_start + (bar_h + 3) * 2);

        label_level = lv_label_create(parent);
        lv_obj_set_style_text_font(label_level, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label_level, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_align(label_level, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(label_level, "Lv1 egg");
        lv_obj_align(label_level, LV_ALIGN_CENTER, 0, bar_y_start + (bar_h + 3) * 3);
    } else {
        /* OLED: no stat bars, no level label (too small) */
    }

    created = true;
    ESP_LOGI(TAG, "Tama render created (scale=%d)", scale);
}

void tama_render_update(tama2_state_t state, const tama2_stats_t *stats, uint8_t critter_idx)
{
    if (!created || !stats) return;

    /* Select sprite frame based on state */
    if (critter_idx >= TAMA_CRITTER_COUNT)
        critter_idx = 0;

    const tama_critter_t *critter = &tama_critters[critter_idx];
    const uint8_t *frame;

    /* Animate: toggle frame every ANIM_FRAME_MS */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((now - last_anim_ms) >= ANIM_FRAME_MS) {
        anim_frame = !anim_frame;
        last_anim_ms = now;

        /* Breathing bounce */
        if (bounce_dir == 0) {
            bounce_offset++;
            if (bounce_offset >= BOUNCE_RANGE) bounce_dir = 1;
        } else {
            bounce_offset--;
            if (bounce_offset <= -BOUNCE_RANGE) bounce_dir = 0;
        }
    }

    /* Select frame: sleeping/sad use idle more, active states use main more */
    if (state == TAMA2_SLEEPING) {
        frame = critter->idle_frame; /* always idle when sleeping */
    } else if (state == TAMA2_EXCITED || state == TAMA2_CELEBRATING) {
        frame = anim_frame ? critter->main_frame : critter->idle_frame; /* fast toggle */
    } else {
        /* Normal: mostly main, occasionally idle */
        frame = anim_frame ? critter->idle_frame : critter->main_frame;
    }

    /* Colors based on state */
    lv_color_t fg, bg;
    if (scr_w >= 240) {
        bg = lv_color_hex(0x000000); /* black background on round */
        switch (state) {
        case TAMA2_HAPPY:       fg = lv_color_hex(0x00FF00); break;
        case TAMA2_EXCITED:     fg = lv_color_hex(0xFFFF00); break;
        case TAMA2_EATING:      fg = lv_color_hex(0xFF8800); break;
        case TAMA2_SICK:        fg = lv_color_hex(0xFF0000); break;
        case TAMA2_SAD:         fg = lv_color_hex(0x8888FF); break;
        case TAMA2_SLEEPING:    fg = lv_color_hex(0x444488); break;
        case TAMA2_CELEBRATING: fg = lv_color_hex(0xFF00FF); break;
        default:               fg = lv_color_hex(0xFFFFFF); break;
        }
    } else {
        /* OLED mono: black bg (OFF), white creature (ON) */
        bg = lv_color_hex(0x000000);
        fg = lv_color_hex(0xFFFFFF);
    }

    if (canvas) {
        /* Round: draw on true-color canvas */
        draw_sprite(frame, fg, bg);
        lv_obj_align(canvas, LV_ALIGN_CENTER, 0, -20 + bounce_offset);
        lv_obj_invalidate(canvas);
    } else if (container) {
        if (!lv_obj_is_valid(container)) { container = NULL; return; }
        /* OLED: copy sprite bits to mono buffer */
        memcpy(mono_buf, frame, TAMA_SPRITE_BYTES);
        lv_img_set_src(container, &mono_img_dsc);
        lv_obj_set_pos(container, 96, 20 + bounce_offset);
        lv_obj_invalidate(container);
    }

    /* Update bars */
    if (bar_hunger && !lv_obj_is_valid(bar_hunger)) bar_hunger = NULL;
    if (bar_happy  && !lv_obj_is_valid(bar_happy))  bar_happy = NULL;
    if (bar_energy && !lv_obj_is_valid(bar_energy)) bar_energy = NULL;
    if (bar_hunger) lv_bar_set_value(bar_hunger, stats->hunger, LV_ANIM_ON);
    if (bar_happy)  lv_bar_set_value(bar_happy, stats->happiness, LV_ANIM_ON);
    if (bar_energy) lv_bar_set_value(bar_energy, stats->energy, LV_ANIM_ON);

    /* Update level */
    if (label_level) {
        lv_label_set_text_fmt(label_level, "Lv%d %s", stats->level + 1, critter->name);
    }
}

void tama_render_set_visible(bool visible)
{
    if (container) {
        if (visible)
            lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    }
}

void tama_render_destroy(void)
{
    /* Don't delete objects — LVGL clean handles that.
       Just reset our pointers so create works again. */
    container = NULL;
    canvas = NULL;
    bar_hunger = NULL;
    bar_happy = NULL;
    bar_energy = NULL;
    label_level = NULL;
    created = false;
}
