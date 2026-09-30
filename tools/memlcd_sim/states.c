#include "states.h"
#include <string.h>

/* chest_proto.h op table (chest_proto.c:chest_op_label), for reference:
 *   0 "" 1 SIGN 2 DECRYP 3 AUTH 4 OTP 5 "FIDO +" 6 FIDO 7 TOTP 8 DELETE
 *   9 REPLAC 10 RESET!
 * chest_mode_label active modes: 1 DISK 2 PGP 3 OTP 4 FIDO 5 TOTP(OATH) */
#define OP_TOTP   7
#define OP_RESET  10

static void base(memlcd_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->is_left = 1;
    m->osl = MEMLCD_OSL_AUCUNE;
    m->batt_local_dv = 0xFF;
}

void scenarios_build(scenario_t out[N_SCENARIOS])
{
    memset(out, 0, sizeof(scenario_t) * N_SCENARIOS);
    scenario_t *s;

    /* 1: USB, BASE layer, full battery, no chest. */
    s = &out[0];
    s->id = "1_usb_base_full"; s->caption = "1  USB . BASE . 100%";
    base(&s->m);
    s->m.route_rf = 0; s->m.dongle_vu = 0;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 42; s->m.batt_niveau = 0; s->batt_pct = 100;

    /* 2: radio on battery 40%, layer NAVIGATION, CAPS on. */
    s = &out[1];
    s->id = "2_rf_navigation_caps"; s->caption = "2  RF . NAVIGATION . 40% . CAPS";
    base(&s->m);
    s->m.route_rf = 1; s->m.dongle_vu = 1;
    strcpy(s->m.nom, "NAVIGATION");
    s->m.batt_local_dv = 37; s->m.batt_niveau = 0; s->batt_pct = 40;
    s->m.caps_lock = 1; s->m.caps_word = 1;   /* mockup: shows the CAPS state regardless of host-LED/USB gating */

    /* 3: low battery + TRRS link. */
    s = &out[2];
    s->id = "3_low_batt_trrs"; s->caption = "3  RF . BASE . LOW BATT . TRRS 5V";
    base(&s->m);
    s->m.route_rf = 1; s->m.dongle_vu = 1;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 34; s->m.batt_niveau = 1; s->batt_pct = 15;
    s->m.lien_5v = 1;

    /* 4: chest present in TOTP mode, USB. */
    s = &out[3];
    s->id = "4_chest_totp_usb"; s->caption = "4  USB . chest present . TOTP mode";
    base(&s->m);
    s->m.route_rf = 0;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 40; s->batt_pct = 92;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_wanted = CHEST_MODE_OATH;
    s->m.coffre_mode_state = CHEST_MODE_ARRIVED;

    /* 5: TOTP browser 3/12 "OVH:PERSO", NO TIME. */
    s = &out[4];
    s->id = "5_totp_browse_notime"; s->caption = "5  TOTP browser 3/12 OVH:PERSO . NO TIME";
    base(&s->m);
    s->m.route_rf = 0;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 39; s->batt_pct = 88;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD;   /* no TIME bit */
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_wanted = CHEST_MODE_OATH;
    s->m.coffre_mode_state = CHEST_MODE_ARRIVED;
    s->m.coffre_browsing = 1; s->m.coffre_pos = 2; s->m.coffre_total = 12;
    strcpy(s->m.coffre_nom, "OVH:PERSO");

    /* 6: prompt TOTP, label "GITHUB". */
    s = &out[5];
    s->id = "6_prompt_totp_github"; s->caption = "6  prompt TOTP . GITHUB";
    base(&s->m);
    s->m.route_rf = 0;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 39; s->batt_pct = 88;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_state = CHEST_MODE_ARRIVED;
    s->m.coffre_op = OP_TOTP; s->m.coffre_op_count = 1;
    strcpy(s->m.coffre_label, "GITHUB");

    /* 7: prompt with a 34-char label. */
    s = &out[6];
    s->id = "7_prompt_totp_longlabel"; s->caption = "7  prompt TOTP . 34-char label";
    s->m = out[5].m;   /* same op/mode as state 6 */
    strcpy(s->m.coffre_label, "GITHUB.COM:ALICE.MARTIN@WORK-2FA01");

    /* 8: RESET! prompt, 12 accounts. */
    s = &out[7];
    s->id = "8_prompt_reset_12"; s->caption = "8  prompt RESET! . 12 accounts";
    base(&s->m);
    s->m.route_rf = 0;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 39; s->batt_pct = 88;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_state = CHEST_MODE_ARRIVED;
    s->m.coffre_op = OP_RESET; s->m.coffre_op_count = 12;
    strcpy(s->m.coffre_label, "NIPHAR_CHEST");

    /* 9: 6-digit code 418902 at 12s for "GITHUB". */
    s = &out[8];
    s->id = "9_code_418902_12s"; s->caption = "9  code 418902 . 12s . GITHUB";
    base(&s->m);
    s->m.route_rf = 0;
    strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 39; s->batt_pct = 88;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_state = CHEST_MODE_ARRIVED;
    s->m.coffre_code_visible = 1;
    strcpy(s->m.coffre_code, "418902"); s->m.coffre_code_secs = 12;
    strcpy(s->m.coffre_nom, "GITHUB");

    /* 10: sleeping (frozen last image). */
    s = &out[9];
    s->id = "10_sleeping"; s->caption = "10  sleeping (frozen)";
    s->m = out[0].m;   /* freeze on the state-1 content */
    s->m.veille = 1;
    s->batt_pct = out[0].batt_pct;
}

void security_scenarios_build(scenario_t out[N_SECURITY])
{
    memset(out, 0, sizeof(scenario_t) * N_SECURITY);
    scenario_t *s;

    s = &out[0];
    s->id = "sec1_prompt_test_rfc"; s->caption = "prompt . TEST:RFC6238";
    base(&s->m); s->m.route_rf = 0; strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 39; s->batt_pct = 88;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_state = CHEST_MODE_ARRIVED;
    s->m.coffre_op = OP_TOTP; s->m.coffre_op_count = 1;
    strcpy(s->m.coffre_label, "TEST:RFC6238");

    s = &out[1];
    s->id = "sec2_prompt_banque_4021"; s->caption = "prompt . BANQUE:4021";
    s->m = out[0].m;
    strcpy(s->m.coffre_label, "BANQUE:4021");

    s = &out[2];
    s->id = "sec3_browse_test_rfc"; s->caption = "browse . TEST:RFC6238";
    base(&s->m); s->m.route_rf = 0; strcpy(s->m.nom, "BASE");
    s->m.batt_local_dv = 39; s->batt_pct = 88;
    s->m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_TIME;
    s->m.coffre_mode_active = CHEST_MODE_OATH; s->m.coffre_mode_state = CHEST_MODE_ARRIVED;
    s->m.coffre_browsing = 1; s->m.coffre_pos = 0; s->m.coffre_total = 5;
    strcpy(s->m.coffre_nom, "TEST:RFC6238");

    s = &out[3];
    s->id = "sec4_browse_banque_4021"; s->caption = "browse . BANQUE:4021";
    s->m = out[2].m;
    s->m.coffre_pos = 1;
    strcpy(s->m.coffre_nom, "BANQUE:4021");
}
