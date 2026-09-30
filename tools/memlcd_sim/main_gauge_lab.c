#include "lvgl.h"
#include "common.h"
#include "gauge_lab.h"
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_12);

static const lv_color_t INK = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static const lv_color_t PAPER = LV_COLOR_MAKE(0x00, 0x00, 0x00);

static void render_g3(void)
{
    char path[256];
    char title[64];
    snprintf(title, sizeof title, "GAUGE G3 water drop -- dark");
    sheet_begin(4, 2, 3, &lv_font_montserrat_12, title);

    /* row 1: the four levels, standalone */
    int pcts[4] = { 100, 60, 30, 10 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *scr = panel_new_screen();
        gauge_lab_standalone(scr, INK, PAPER, (uint8_t)pcts[i], false);
        panel_capture();
        char cap[16]; snprintf(cap, sizeof cap, "%d%%", pcts[i]);
        sheet_add(cap);
    }

    /* row 2: state 1 (USB, BASE, 100%), state 2 (RADIO, SEEN, NAVIGATION
     * wrapped, CAPS CW, 40%), low battery (RADIO, BASE, 15%, 3.4V) --
     * scenario values match states.c scenarios 1/2/3 exactly. */
    lv_obj_t *scr;

    scr = panel_new_screen();
    gauge_lab_mockup(scr, INK, PAPER, false, false, "BASE", NULL, NULL, 100, 42, 0, false);
    panel_capture();
    sheet_add("state 1: USB . BASE . 100%");

    scr = panel_new_screen();
    gauge_lab_mockup(scr, INK, PAPER, true, true, "NAVIG", "ATION", "CAPS CW", 40, 37, 0, false);
    panel_capture();
    sheet_add("state 2: RADIO . SEEN . NAVIGATION . CAPS . 40%");

    scr = panel_new_screen();
    gauge_lab_mockup(scr, INK, PAPER, true, true, "BASE", NULL, NULL, 15, 34, 0, true);
    panel_capture();
    sheet_add("LOW: RADIO . BASE . 3.4V . 15%");

    char path2[256];
    snprintf(path, sizeof path, "out/gauges/gauge_G3.png");
    sheet_end(path);
    (void)path2;
}

int main(void)
{
    sim_init();
    render_g3();
    printf("Gauge lab: done\n");
    return 0;
}
