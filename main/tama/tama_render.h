/* Tamagotchi LVGL renderer — draws the pet and stat bars.
   Adapts to screen size (240x240 round or 128x64 OLED).
   Coexists with the existing UI (layer name, BT status, etc). */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "tama_engine.h"

/* Create tama UI elements on the given parent (call once after display init) */
void tama_render_create(lv_obj_t *parent, uint16_t screen_w, uint16_t screen_h);

/* Update the rendered pet (call periodically with current state/stats).
   Must be called with lvgl_port_lock held. */
void tama_render_update(tama2_state_t state, const tama2_stats_t *stats, uint8_t critter_idx);

/* Show/hide the tama UI */
void tama_render_set_visible(bool visible);

/* Destroy tama UI (on disable) */
void tama_render_destroy(void);
