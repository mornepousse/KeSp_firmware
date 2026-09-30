/* Renders the left screen with the FIRMWARE's engine (main/display/memlcd/
 * memlcd_cave.c, compiled as is): the 10 reference states of the design
 * brief, the status band (the drop at 100..5 %, LOW, charging, FULL, "?";
 * the route and caps icons), the countdown draining, and
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

    /* The status band's drop (spec "Status icons", 2026-09-30), on the
     * normal screen of state 1: the percentage inside only at <= 15 %. */
    sheet_begin(7, 2, 3, &lv_font_montserrat_12, "CAVE / DARK -- status band: the drop");
    static const uint8_t pcts[7] = { 100, 60, 30, 20, 15, 10, 5 };
    for (int i = 0; i < 7; i++) {
        frame(&sc[0].m, pcts[i]);
        char cap[16]; snprintf(cap, sizeof cap, "%u%%", pcts[i]);
        sheet_add(cap);
    }
    { memlcd_model_t m = sc[0].m; m.batt_niveau = 1; frame(&m, 15); }
    sheet_add("15% LOW");
    { memlcd_model_t m = sc[0].m; m.batt_niveau = 2; frame(&m, 5); }
    sheet_add("5% CRITICAL");
    { memlcd_model_t m = sc[0].m; m.batt_local_chg = 1; frame(&m, 40); }
    sheet_add("USB charging 40%");
    { memlcd_model_t m = sc[0].m; m.batt_local_chg = 1; frame(&m, 90); }
    sheet_add("USB charging 90%");
    { memlcd_model_t m = sc[0].m; m.batt_local_chg = 2; frame(&m, 95); }
    sheet_add("USB FULL");
    { memlcd_model_t m = sc[0].m; m.batt_local_dv = 0xFF; frame(&m, 0xFF); }
    sheet_add("unknown ?");
    { memlcd_model_t m = sc[0].m; m.route_rf = 1; m.dongle_vu = 1; m.lien_5v = 1; frame(&m, 10); }
    sheet_add("RF seen + TRRS 10%");
    sheet_end("out/cave_dark/anim_battery_cave_dark.png");

    /* The route icons and the caps icons. */
    sheet_begin(6, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- route and caps icons");
    frame(&sc[0].m, 80);
    sheet_add("USB");
    { memlcd_model_t m = sc[0].m; m.route_rf = 1; m.dongle_vu = 1; frame(&m, 80); }
    sheet_add("RF, dongle seen");
    { memlcd_model_t m = sc[0].m; m.route_rf = 1; m.dongle_vu = 0; frame(&m, 80); }
    sheet_add("RF, not seen");
    { memlcd_model_t m = sc[0].m; m.caps_lock = 1; frame(&m, 80); }
    sheet_add("Caps Lock");
    { memlcd_model_t m = sc[0].m; m.caps_word = 1; frame(&m, 80); }
    sheet_add("Caps Word");
    { memlcd_model_t m = sc[0].m; m.caps_lock = 1; m.caps_word = 1; m.osm = 0x02; m.osl = 3; frame(&m, 80); }
    sheet_add("both + S L3");
    sheet_end("out/cave_dark/icons_cave_dark.png");

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

    sheet_begin(2, 3, 3, &lv_font_montserrat_12, "CAVE / DARK -- security proof: no unmarked digit-only line");
    for (int i = 0; i < N_SECURITY; i++) {
        frame(&sec[i].m, sec[i].batt_pct);
        snprintf(path, sizeof path, "out/cave_dark/%s.png", sec[i].id);
        panel_save_png(path, 4);
        sheet_add(sec[i].caption);
    }
    sheet_end("out/cave_dark/security_proof_cave_dark.png");

    /* The bench states of plan Task 3: an 8-digit code, a 34 'W' label (the
     * UNSCII last resort) and a 34-character digit-heavy label (marked). */
    sheet_begin(4, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- bench extras");
    { memlcd_model_t m = sc[8].m; strcpy(m.coffre_nom, "Jetable:huit"); strcpy(m.coffre_code, "12345678"); frame(&m, 88); }
    sheet_add("code 8 digits . Jetable:huit");
    { memlcd_model_t m = sc[5].m; strcpy(m.coffre_label, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"); m.coffre_op_count = 12; frame(&m, 88); }
    sheet_add("prompt . 34 W . 12 accts");
    { memlcd_model_t m = sc[5].m; strcpy(m.coffre_label, "AWS:123456789012345678901234567890"); m.coffre_op_count = 12; frame(&m, 88); }
    sheet_add("prompt . 34 digit-heavy . 12");
    { memlcd_model_t m = sc[3].m; m.coffre &= (uint8_t)~CHEST_STATE_SD; m.dongle_vu = 1; m.caps_lock = 1; m.caps_word = 1; m.osm = 0x02; m.osl = 3; strcpy(m.nom, "NAVIGATION"); frame(&m, 50); }
    sheet_add("everything at once");
    sheet_end("out/cave_dark/bench_extras_cave_dark.png");

    printf("Cave / Dark (firmware engine): done\n");
    return 0;
}
