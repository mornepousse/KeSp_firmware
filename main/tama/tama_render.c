/* Tamagotchi LVGL renderer */
#include "tama_render.h"
#include "tama_sprites.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TAMA_RENDER";

/* ── UI elements ─────────────────────────────────────────────────── */

static lv_obj_t *container = NULL;  /* main container */
static lv_obj_t *canvas = NULL;     /* sprite canvas */
static lv_obj_t *bar_hunger = NULL;
static lv_obj_t *bar_happy = NULL;
static lv_obj_t *bar_energy = NULL;
static lv_obj_t *label_level = NULL;

/* Canvas buffer — keep small to avoid RAM issues with BLE stack */
static lv_color_t canvas_buf[32 * 32]; /* no upscale in canvas, LVGL zoom handles it */

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
    uint16_t sprite_size = TAMA_SPRITE_W;

    /* Simple: just a label for the critter + level (no canvas for now) */
    label_level = lv_label_create(parent);
    lv_label_set_text(label_level, "Lv1 egg");
    lv_obj_set_style_text_color(label_level, lv_color_hex(0x00FF00), 0);
    lv_obj_align(label_level, LV_ALIGN_CENTER, 0, 30);

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

    /* Alternate main/idle based on state */
    if (state == TAMA2_IDLE || state == TAMA2_SLEEPY || state == TAMA2_SLEEPING || state == TAMA2_SAD) {
        frame = critter->idle_frame;
    } else {
        frame = critter->main_frame;
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
        bg = lv_color_hex(0x000000); /* OLED mono */
        fg = lv_color_hex(0xFFFFFF);
    }

    draw_sprite(frame, fg, bg);
    lv_obj_invalidate(canvas);

    /* Update bars */
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
    if (container) {
        lv_obj_del(container);
        container = NULL;
        canvas = NULL;
        bar_hunger = NULL;
        bar_happy = NULL;
        bar_energy = NULL;
        label_level = NULL;
        created = false;
    }
}
