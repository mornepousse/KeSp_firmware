/*
 * Dongle state — what it still has left to carry.
 *
 * This file used to be called dongle_engine_state.c and manufactured the
 * globals that matrix_scan.c provides on a keyboard: MATRIX_STATE, keycodes[],
 * current_press_*, stat_matrix_changed. It only existed to run the keymap
 * engine on half-matrices received over radio — architecture A.
 *
 * The Niphargus sends already-finished HID, so the dongle no longer decodes
 * any matrix and no longer runs an engine. See
 * docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md
 *
 * Only three things that still make sense on a repeater remain here: the HID
 * output mode, the current layer reported to the monitor, and the battery
 * cache for both slots — the "it knows and it reports" part of its role.
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "sdkconfig.h"   /* CONFIG_KASE_DONGLE_FUSION: the dongle regains the engine */

/* HID output mode (0 = USB). The dongle has no BLE, but hid_transport.c reads
 * this global on every role. */
uint8_t usb_bl_state = 0;

/* Current layer — reported as-is to the CDC monitor. Outside fusion the
 * dongle has no engine, it never changes on its own; under fusion the engine
 * (key_processor) writes it, but its DEFINITION lives here regardless: on a
 * keyboard it is in matrix_scan.c, not compiled on the dongle (no local
 * matrix). Unconditional, therefore. */
uint8_t current_layout = 0;

/* ── Engine state symbols, FUSION mode ────────────────────────────────────
 * Under KASE_DONGLE_FUSION the dongle runs the keymap engine. It therefore
 * compiles the KEYBOARD block of the CDC protocol, which references globals
 * that matrix_scan.c provides on a keyboard — but it has no local matrix.
 * We provide them here, as cdc_split_scanner_stubs.c does for the right half.
 * matrix_test_*: the matrix test command is inert without a matrix.
 * layer_changed: no screen to notify on this role (for now). */
#if CONFIG_KASE_DONGLE_FUSION
volatile bool     matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
void layer_changed(void) { /* no screen or link to notify here (phase 1) */ }
#endif

/* ── Battery cache for both slots ─────────────────────────────────────────
 * Indexed like the RF slots: 0 = keyboard, 1 = mouse (comm/rf/rf_slot.h).
 * Fed by the status frame received from each device, read by the CDC BATTERY
 * command. This is supervision: the dongle knows and reports, it decides nothing.
 *
 * The status frame only carries the voltage — four bytes was a design
 * constraint, not an oversight. The charge state and ongoing charging thus
 * stay at 0xFF "unknown" rather than being guessed. */
typedef struct {
    uint8_t  batt_dV;     /* 0xFF = unknown */
    uint8_t  soc_pct;     /* 0xFF = unknown */
    uint8_t  charging;    /* 0xFF = unknown */
    uint32_t last_ms;     /* 0 = never seen */
} dongle_batt_t;

/* Index = HALF (0 left, 1 right) — cf. cache_battery_half (rf_rx_task.c). */
static dongle_batt_t s_batt[2] = {
    { 0xFF, 0xFF, 0xFF, 0 },
    { 0xFF, 0xFF, 0xFF, 0 },
};

void dongle_cache_set_battery(uint8_t slot,
                              uint8_t batt_dV, uint8_t soc_pct, uint8_t charging)
{
    if (slot > 1) return;
    s_batt[slot].batt_dV  = batt_dV;
    s_batt[slot].soc_pct  = soc_pct;
    s_batt[slot].charging = charging;
    s_batt[slot].last_ms  = (uint32_t)(esp_timer_get_time() / 1000);
}

void dongle_cache_get_battery(uint8_t slot,
                              uint8_t *batt_dV, uint8_t *soc_pct,
                              uint8_t *charging, uint32_t *age_ms_out)
{
    uint8_t s = (slot > 1) ? 1 : slot;
    *batt_dV  = s_batt[s].batt_dV;
    *soc_pct  = s_batt[s].soc_pct;
    *charging = s_batt[s].charging;
    if (s_batt[s].last_ms == 0) {
        *age_ms_out = 0xFFFFFFFFu;
    } else {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        *age_ms_out = now - s_batt[s].last_ms;
    }
}

/* ── CDC monitor fields ────────────────────────────────────────────────────
 * The dongle counts no keystrokes: it relays already-finished HID, never
 * looking at what it contains. These two values stay exposed, at zero, rather
 * than amputating the monitor's frame format — the control software thus
 * reads the same fields regardless of the device at the other end of the cable.
 *
 * Under FUSION the engine is really there: key_stats.c defines
 * key_stats_total and key_features.c defines wpm_get(). We only stub them out
 * in the absence of an engine, otherwise the link would have a double definition. */
#if !CONFIG_KASE_DONGLE_FUSION
uint32_t key_stats_total = 0;

uint16_t wpm_get(void) { return 0; }
#endif
