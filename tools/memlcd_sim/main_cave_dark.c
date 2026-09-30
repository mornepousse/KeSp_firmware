#include "lvgl.h"
#include "common.h"
#include "states.h"
#include "dir_cave_dark.h"
#include "cave_ui.h"
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_12);

int main(void)
{
    sim_init();
    scenario_t sc[N_SCENARIOS];
    scenarios_build(sc);
    scenario_t sec[N_SECURITY];
    security_scenarios_build(sec);

    lv_obj_t *scr = panel_new_screen();
    dir_cave_dark_build(scr);

    char path[256];
    sheet_begin(2, 5, 3, &lv_font_montserrat_12, "CAVE / DARK -- inverted, pale ink on black");
    for (int i = 0; i < N_SCENARIOS; i++) {
        dir_cave_dark_draw(&sc[i].m, sc[i].batt_pct);
        panel_capture();
        snprintf(path, sizeof path, "out/cave_dark/%s.png", sc[i].id);
        panel_save_png(path, 4);
        sheet_add(sc[i].caption);
    }
    sheet_end("out/cave_dark/contact_sheet_cave_dark.png");

    sheet_begin(4, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- battery water level");
    int pcts[4] = { 100, 60, 30, 10 };
    for (int i = 0; i < 4; i++) {
        cave_draw_battery_demo(dir_cave_dark_ui(), (uint8_t)pcts[i]);
        panel_capture();
        char cap[32]; snprintf(cap, sizeof cap, "%d%%", pcts[i]);
        sheet_add(cap);
    }
    sheet_end("out/cave_dark/anim_battery_cave_dark.png");

    sheet_begin(4, 1, 3, &lv_font_montserrat_12, "CAVE / DARK -- TOTP countdown water level");
    int secs[4] = { 12, 9, 6, 3 };
    for (int i = 0; i < 4; i++) {
        cave_draw_code_demo(dir_cave_dark_ui(), "GITHUB", "418902", (uint8_t)secs[i]);
        panel_capture();
        char cap[32]; snprintf(cap, sizeof cap, "t=%ds", secs[i]);
        sheet_add(cap);
    }
    sheet_end("out/cave_dark/anim_countdown_cave_dark.png");

    sheet_begin(2, 2, 3, &lv_font_montserrat_12, "CAVE / DARK -- security proof: no digit-only line");
    for (int i = 0; i < N_SECURITY; i++) {
        dir_cave_dark_draw(&sec[i].m, sec[i].batt_pct);
        panel_capture();
        sheet_add(sec[i].caption);
    }
    sheet_end("out/cave_dark/security_proof_cave_dark.png");

    printf("Cave / Dark: done\n");
    return 0;
}
