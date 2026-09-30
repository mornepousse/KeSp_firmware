#pragma once
#include "memlcd_model.h"

/* The ten canonical scenarios from the design brief, built directly as
 * memlcd_model_t values (no live chest/radio link — this is a UI mockup,
 * not a protocol test). batt_pct is an explicit gauge percentage supplied
 * alongside batt_local_dv, so the mockup does not need to pull in
 * batt_calc.c's real discharge curve. */
typedef struct {
    const char *id;         /* filename stem, e.g. "1_usb_base_full" */
    const char *caption;    /* one-line description for the contact sheet */
    memlcd_model_t m;
    uint8_t batt_pct;
} scenario_t;

#define N_SCENARIOS 10
void scenarios_build(scenario_t out[N_SCENARIOS]);

/* Security proof set (Mae, 2026-09-29 bench incident): a prompt and a
 * browser entry for each of the two adversarial labels — "TEST:RFC6238"
 * (must not cut to "TEST:RFC" / "6238") and "BANQUE:4021" (must not cut to
 * "BANQUE:" / "4021" either — the colon does not make a digit run safe). */
#define N_SECURITY 4
void security_scenarios_build(scenario_t out[N_SECURITY]);
