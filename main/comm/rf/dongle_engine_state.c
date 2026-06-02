/*
 * Engine input-state globals for the dongle.
 * On keyboards these live in matrix_scan.c (not compiled on the dongle).
 * rf_rx_task.c populates current_press_row/col[] + MATRIX_STATE from RF;
 * the engine (build_keycode_report) consumes them exactly as on a keyboard.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "keyboard_config.h"   /* MATRIX_ROWS, MATRIX_COLS */

#define MAX_REPORT_KEYS 6      /* must match matrix_scan.c boot-protocol limit */

uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];  /* symbol parity; unused on dongle */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];
volatile uint8_t stat_matrix_changed;

uint8_t current_layout = 0;
uint8_t last_layer = 0;

/* HID output routing: 0 = USB (dongle is USB-only, always 0). */
uint8_t usb_bl_state = 0;

/*
 * On keyboards layer_changed() flags the display task.
 * On the dongle: push EN_INFO_LAYER + EN_INFO_STATE to both halves via ESP-NOW.
 */
#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_link.h"
#include "espnow_msg.h"
#include "nvs.h"                    /* nvs_open, nvs_get_blob, nvs_close */
#include "hid_report.h"             /* hid_report_get_modifiers() */
#include "rf_rx_task.h"             /* rf_rx_copy_peer_macs() — live peer MACs */
#include "keyboard_config.h"        /* LAYERS, MAX_LAYOUT_NAME_LENGTH */
#include "keymap.h"                 /* default_layout_names */
#include "esp_log.h"
#endif

void layer_changed(void)
{
#if CONFIG_KASE_HAS_ESPNOW
    /* Live paired-half MACs from the RX task (loaded at boot + refreshed on
     * every pairing). Avoids a stale one-shot NVS cache that required a dongle
     * reboot before layer/state push would resume after a re-pairing. */
    uint8_t mac_left[6], mac_right[6];
    rf_rx_copy_peer_macs(mac_left, mac_right);

    bool has_left  = mac_left[0]  || mac_left[1]  || mac_left[2]  ||
                     mac_left[3]  || mac_left[4]  || mac_left[5];
    bool has_right = mac_right[0] || mac_right[1] || mac_right[2] ||
                     mac_right[3] || mac_right[4] || mac_right[5];

    if (!has_left && !has_right) {
        ESP_LOGD("dongle_engine", "layer_changed: no paired halves — ESP-NOW skip");
        return;
    }

    /* ── Build EN_INFO_LAYER payload ────────────────────────────
     * default_layout_names: char[LAYERS][MAX_LAYOUT_NAME_LENGTH] (15 bytes each).
     * en_layer_t.name:  char[16]. Copy 15 bytes max; 16th byte stays NUL. */
    en_layer_t l;
    memset(&l, 0, sizeof(l));
    l.layer_idx = current_layout;
    if (current_layout < LAYERS) {
        strncpy(l.name, default_layout_names[current_layout], 15);
        /* l.name[15] is already 0 from memset */
    }

    /* ── Build EN_INFO_STATE payload ────────────────────────────
     * espnow_send() is called from rf_rx_task context — esp_now_send()
     * is internally queued and thread-safe (spec §7.1 rationale). */
    en_state_t s;
    memset(&s, 0, sizeof(s));
    s.modifiers = hid_report_get_modifiers();
    s.flags     = 0;   /* caps_word / bt flags: future expansion (spec §7.1) */

    /* ── Unicast to each paired half — fire-and-forget ──────────
     * Low rate (one per layer change, typically seconds apart).
     * NRF is NOT muted: info channel duty cycle < 0.1% (spec §7.5).
     * espnow_send() returns false gracefully if peer was not added. */
    if (has_left) {
        espnow_send(mac_left, EN_INFO_LAYER, &l, sizeof(l));
        espnow_send(mac_left, EN_INFO_STATE, &s, sizeof(s));
    }
    if (has_right) {
        espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof(l));
        espnow_send(mac_right, EN_INFO_STATE, &s, sizeof(s));
    }
#endif /* CONFIG_KASE_HAS_ESPNOW */
}

/* matrix test mode is keyboard-only; inert globals so CDC handlers link */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;

/* ── Battery sample cache (dongle) ─────────────────────────────────
 * Filled by espnow_info.c::on_battery() each time a half pushes
 * EN_INFO_BATTERY. Drained by the CDC KS_CMD_BATTERY handler.
 *
 * Slot mapping mirrors rf_link_status_t: 0 = LEFT, 1 = RIGHT.
 * dV/soc/charging default to 0xFF until the first sample arrives → the
 * controller can render "unknown" without polling forever.
 * last_ms = esp_timer_get_time()/1000 at capture; 0 = never. */
#include "esp_timer.h"

typedef struct {
    uint8_t  batt_dV;     /* en_battery_t.batt_dV   (0xFF = unknown) */
    uint8_t  soc_pct;     /* en_battery_t.soc_pct   (0xFF = unknown) */
    uint8_t  charging;    /* en_battery_t.charging  (0xFF = unknown) */
    uint32_t last_ms;     /* esp_timer ms at last update; 0 = never */
} dongle_batt_t;

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
        *age_ms_out = 0xFFFFFFFFu;  /* never seen */
    } else {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        *age_ms_out = now - s_batt[s].last_ms;
    }
}
