/* Renders the RIGHT half's screen with the FIRMWARE's engine: memlcd_cave.c
 * compiled as is with MEMLCD_CAVE_DROITE=1, exactly as main/CMakeLists.txt
 * compiles it for niphar_right, and every frame through memlcd_cave_draw as
 * memlcd_backend.c's dessiner() calls it. The model is filled the way the
 * backend's lire_modele() fills it on the right: memlcd_droite_route() from
 * the USB presence and the dongle's ACK. */
#include "lvgl.h"
#include "common.h"
#include "memlcd_cave.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_12);

typedef struct {
    const char *id, *caption;
    bool usb, vu;
    uint8_t pct, niveau, chg, lien, veille, dv;
} etat_droite_t;

static const etat_droite_t etats[] = {
    { "r1_usb_100",          "USB 100%",    true, false, 100, 0, 0, 0, 0, 42 },
    { "r2_radio_seen_60",    "seen 60%",    false, true, 60, 0, 0, 0, 0, 39 },
    { "r3_radio_unseen_30",  "unseen 30%",  false, false, 30, 0, 0, 0, 0, 37 },
    { "r4_low_10_trrs",      "LOW10 TRRS",  false, true, 10, 1, 0, 1, 0, 34 },
    { "r5_usb_charging_40",  "charge 40%",  true, false, 40, 0, 1, 0, 0, 38 },
    { "r6_usb_full",         "USB FULL",    true, false, 95, 0, 2, 0, 0, 42 },
    { "r7_unknown",          "unknown",     false, true, 0xFF, 0, 0, 0, 0, 0xFF },
    { "r8_critical_5",       "CRIT 5%",     false, false, 5, 2, 0, 0, 0, 33 },
    { "r9_sleep",            "sleep",       false, true, 60, 0, 0, 0, 1, 39 },
};
#define N_ETATS ((int)(sizeof etats / sizeof etats[0]))

int main(void)
{
    sim_init();
    lv_obj_t *scr = panel_new_screen();
    memlcd_cave_build(scr);

    char path[256];
    sheet_begin(5, 2, 3, &lv_font_montserrat_12, "RIGHT half -- cave, DROITE");
    for (int i = 0; i < N_ETATS; i++) {
        const etat_droite_t *e = &etats[i];
        memlcd_model_t m;
        memset(&m, 0, sizeof m);
        m.osl = MEMLCD_OSL_AUCUNE;
        m.batt_local_dv = e->dv; m.batt_pct = e->pct;
        m.batt_niveau = e->niveau; m.batt_local_chg = e->chg;
        m.lien_5v = e->lien; m.veille = e->veille;
        memlcd_droite_route(&m, e->usb, e->vu);
        memlcd_cave_draw(&m, m.batt_pct);
        panel_capture();
        snprintf(path, sizeof path, "out/cave_right/%s.png", e->id);
        panel_save_png(path, 4);
        sheet_add(e->caption);
    }
    sheet_end("out/cave_right/contact_sheet_cave_right.png");
    printf("Cave / Dark, right half (firmware engine): done\n");
    return 0;
}
