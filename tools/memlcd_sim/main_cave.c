/* Renders the left screen with the FIRMWARE's engine (main/display/memlcd/
 * memlcd_cave.c, compiled as is): the 10 reference states of the design
 * brief, the water drop at four levels and low, the countdown draining, and
 * the security proof (the bench incident's labels on a prompt and in the
 * browser). Every frame goes through memlcd_cave_draw, exactly as
 * memlcd_backend.c's dessiner() calls it. */
#include "lvgl.h"
#include "common.h"
#include "states.h"
#include "memlcd_cave.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_12);

static void frame(const memlcd_model_t *m, uint8_t pct)
{
    memlcd_cave_draw(m, pct);
    panel_capture();
}

int main(void)
{
    sim_init();
    scenario_t sc[N_SCENARIOS];
    scenarios_build(sc);
    scenario_t sec[N_SECURITY];
    security_scenarios_build(sec);

    lv_obj_t *scr = panel_new_screen();
    memlcd_cave_build(scr);

    char path[256];
    sheet_begin(2, 5, 3, &lv_font_montserrat_12, "CAVE / DARK -- firmware memlcd_cave.c");
    for (int i = 0; i < N_SCENARIOS; i++) {
        frame(&sc[i].m, sc[i].batt_pct);
        snprintf(path, sizeof path, "out/cave_dark/%s.png", sc[i].id);
        panel_save_png(path, 4);
        sheet_add(sc[i].caption);
    }
    sheet_end("out/cave_dark/contact_sheet_cave_dark.png");

    /* The water drop, on the normal screen of state 1. */
    sheet_begin(5, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- water drop");
    static const uint8_t pcts[5] = { 100, 60, 30, 10, 10 };
    for (int i = 0; i < 5; i++) {
        memlcd_model_t m = sc[0].m;
        m.batt_niveau = i == 4;
        frame(&m, pcts[i]);
        char cap[32]; snprintf(cap, sizeof cap, "%u%%%s", pcts[i], i == 4 ? " LOW" : "");
        sheet_add(cap);
    }
    sheet_end("out/cave_dark/anim_battery_cave_dark.png");

    /* The gauge sheet of round 4 (G3), from the firmware: the drop at four
     * levels, then the three top-block states it was judged on. */
    sheet_begin(4, 2, 3, &lv_font_montserrat_12, "GAUGE G3 water drop -- firmware");
    for (int i = 0; i < 4; i++) {
        frame(&sc[0].m, pcts[i]);
        char cap[16]; snprintf(cap, sizeof cap, "%u%%", pcts[i]);
        sheet_add(cap);
    }
    frame(&sc[0].m, 100);
    sheet_add("state 1: USB . BASE . 100%");
    { memlcd_model_t m = sc[1].m; m.caps_lock = 1; frame(&m, 40); }
    sheet_add("state 2: RADIO . SEEN . NAVIGATION . CAPS . 40%");
    { memlcd_model_t m = sc[2].m; m.lien_5v = 0; frame(&m, 15); }
    sheet_add("LOW: RADIO . BASE . 15%");
    sheet_end("out/gauges/gauge_G3.png");

    sheet_begin(4, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- TOTP countdown");
    static const uint8_t secs[4] = { 12, 9, 6, 3 };
    for (int i = 0; i < 4; i++) {
        memlcd_model_t m = sc[8].m;
        m.coffre_code_secs = secs[i];
        frame(&m, sc[8].batt_pct);
        char cap[16]; snprintf(cap, sizeof cap, "t=%us", secs[i]);
        sheet_add(cap);
    }
    sheet_end("out/cave_dark/anim_countdown_cave_dark.png");

    sheet_begin(2, 2, 3, &lv_font_montserrat_12, "CAVE / DARK -- security proof: no digit-only line");
    for (int i = 0; i < N_SECURITY; i++) {
        frame(&sec[i].m, sec[i].batt_pct);
        snprintf(path, sizeof path, "out/cave_dark/%s.png", sec[i].id);
        panel_save_png(path, 4);
        sheet_add(sec[i].caption);
    }
    sheet_end("out/cave_dark/security_proof_cave_dark.png");

    /* The bench states of plan Task 3: an 8-digit code, and a 34 'W' label
     * (the UNSCII last resort). */
    sheet_begin(3, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- bench extras");
    { memlcd_model_t m = sc[8].m; strcpy(m.coffre_nom, "Jetable:huit"); strcpy(m.coffre_code, "12345678"); frame(&m, 88); }
    sheet_add("code 8 digits . Jetable:huit");
    { memlcd_model_t m = sc[5].m; strcpy(m.coffre_label, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"); m.coffre_op_count = 12; frame(&m, 88); }
    sheet_add("prompt . 34 W . 12 accts");
    { memlcd_model_t m = sc[3].m; m.coffre &= (uint8_t)~CHEST_STATE_SD; m.dongle_vu = 1; m.caps_lock = 1; m.caps_word = 1; m.osm = 0x02; m.osl = 3; strcpy(m.nom, "NAVIGATION"); frame(&m, 50); }
    sheet_add("everything at once");
    sheet_end("out/cave_dark/bench_extras_cave_dark.png");

    printf("Cave / Dark (firmware engine): done\n");
    return 0;
}
