/*
 * Moteur keymap embarqué dans le dongle — mode FUSION (KASE_DONGLE_FUSION).
 *
 * Voir dongle_engine.h et le design 2026-09-12-dongle-fusion. Le dongle reçoit
 * les deux demi-matrices BRUTES, les fusionne (fusion_state) et fait tourner le
 * MÊME moteur keymap que la gauche, puis sort le HID par son USB.
 *
 * ── État moteur ──
 * Sur un clavier, matrix_scan.c fournit MATRIX_STATE, keycodes[], current_press_*
 * et les drapeaux de couche/activité. Le dongle n'a pas de matrice locale : ce
 * fichier fournit ces globales à leur place. build_keycode_report() les lit ;
 * tant que personne ne l'appelait, le linker les éliminait (gc-sections) — c'est
 * cet appel-ci qui les rend enfin nécessaires.
 *
 * ── Concurrence ──
 * rf_rx_task dépose les trames via dongle_engine_on_matrix() ; la tâche moteur
 * les consomme. SEUL l'état de fusion (s_fusion + s_dirty) est partagé entre les
 * deux — protégé par s_mux. current_press_* et tout le cycle moteur ne sont
 * touchés QUE par la tâche moteur, donc sans verrou.
 */
#include "dongle_engine.h"
#if CONFIG_KASE_DONGLE_FUSION

#include "half_link.h"          /* fusion_state_t, fusion_apply/timeout/collect */
#include "key_processor.h"      /* build_keycode_report, process_matrix_changes, taps */
#include "hid_report.h"         /* send_hid_key */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "key_features.h"       /* key_override_init */
#include "keymap.h"             /* macros_list, MACRO_* */
#include "matrix_scan.h"        /* extern des globales que l'on définit ici */
#include "hid_transport.h"      /* hid_send_keyboard (send_tap) */
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

#ifndef BOARD_REMOTE_COLS_MIRRORED
#define BOARD_REMOTE_COLS_MIRRORED 0
#endif

static const char *TAG = "dongle_eng";

/* ── Globales d'état moteur (fournies par matrix_scan.c sur un clavier) ────── */
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t keycodes[6];
uint8_t current_press_row[6];
uint8_t current_press_col[6];
uint8_t current_press_stat[6];
volatile uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
volatile uint8_t is_layer_changed = 0;
volatile uint32_t last_activity_time_ms = 0;

uint32_t get_last_activity_time_ms(void) { return last_activity_time_ms; }

/* ── État de fusion partagé rf_rx_task ↔ tâche moteur ─────────────────────── */
static fusion_state_t   s_fusion;
static bool             s_dirty;     /* une demi-matrice a changé depuis le dernier cycle */
static SemaphoreHandle_t s_mux;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Émet une frappe brève (press+release), pour tap-dance / leader / macros. */
static void send_tap(uint8_t kc, uint8_t mod)
{
    uint8_t buf[6] = { kc };
    hid_send_keyboard(mod, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(buf, 0, 6);
    hid_send_keyboard(0, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* Recopie l'état fusionné dans current_press_* (le format que lit le moteur).
 * Appelée sous s_mux (lit s_fusion). Gauche en colonnes directes, droite en
 * colonnes hautes via le miroir du PCB — exactement matrix_apply_remote() mais
 * pour DEUX moitiés distantes. */
static void fill_current_press_locked(void)
{
    uint8_t rows[6], cols[6];
    uint8_t n = fusion_collect(&s_fusion, MATRIX_COLS, BOARD_REMOTE_COLS_MIRRORED,
                               rows, cols, 6);
    for (uint8_t i = 0; i < 6; i++) {
        if (i < n) {
            current_press_row[i]  = rows[i];
            current_press_col[i]  = cols[i];
            current_press_stat[i] = 1;
        } else {
            current_press_row[i]  = INVALID_KEY_POS;
            current_press_col[i]  = INVALID_KEY_POS;
            current_press_stat[i] = 0;
        }
    }
}

void dongle_engine_on_matrix(const rf_matrix_t *m)
{
    if (!s_mux) return;
    const half_state_t *hs = (m->half == RF_HALF_RIGHT) ? &s_fusion.right
                           : (m->half == RF_HALF_LEFT)  ? &s_fusion.left : NULL;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool changed = hs && memcmp(hs->bitmap, m->bitmap, RF_HALF_BITMAP_BYTES) != 0;
    if (fusion_apply(&s_fusion, m, now_ms()) && changed)
        s_dirty = true;
    xSemaphoreGive(s_mux);
}

/* Un cycle moteur complet : fusion→current_press déjà fait, on décode et on émet.
 * Calqué sur la branche « matrice changée » de keyboard_task.c. */
static void run_cycle(void)
{
    build_keycode_report();
    process_matrix_changes();
    if (key_processor_has_tap()) {
        send_hid_key();
        vTaskDelay(pdMS_TO_TICKS(10));
        key_processor_clear_taps();
        send_hid_key();
    } else {
        send_hid_key();
    }
    last_activity_time_ms = now_ms();
}

static void dongle_engine_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 10 ms : cadence des minuteries tap-hold / tap-dance, comme le clavier. */
        vTaskDelay(pdMS_TO_TICKS(10));
        tap_hold_tick();
        tap_dance_tick();

        /* Section critique minimale : expiration + snapshot du besoin de recalcul. */
        bool do_cycle = false;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (fusion_timeout(&s_fusion, now_ms(), HALF_LINK_TIMEOUT_MS))
            s_dirty = true;
        if (s_dirty) {
            fill_current_press_locked();
            s_dirty = false;
            do_cycle = true;
        }
        xSemaphoreGive(s_mux);

        if (do_cycle)
            run_cycle();

        /* Maintien qui vient de basculer en « hold » → réémettre. */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

        /* Tap-dance résolu → tap. */
        if (tap_dance_just_resolved()) {
            uint8_t kc = tap_dance_consume();
            if (kc != 0) send_tap(kc, 0);
        }

        /* Leader : timeout + résultat. */
        if (leader_tick()) {
            uint8_t mod = 0;
            uint8_t kc = leader_consume(&mod);
            if (kc != 0) send_tap(kc, mod);
        }

        /* Macro en attente → jouer la séquence (mêmes règles que keyboard_task). */
        if (key_processor_has_pending_macro()) {
            int16_t idx = key_processor_consume_macro();
            if (idx >= 0 && idx < MAX_MACROS) {
                const macro_step_t *steps = macros_list[idx].steps;
                uint8_t pending_mod = 0;
                for (int s = 0; s < MACRO_MAX_STEPS && steps[s].keycode != 0; s++) {
                    uint8_t kc  = steps[s].keycode;
                    uint8_t mod = steps[s].modifier;
                    if (kc == MACRO_DELAY_MARKER) { vTaskDelay(pdMS_TO_TICKS(mod * 10)); continue; }
                    if (kc >= 0xE0 && kc <= 0xE7) { pending_mod |= (1 << (kc - 0xE0)); continue; }
                    send_tap(kc, mod | pending_mod);
                    pending_mod = 0;
                }
            }
        }
    }
}

void dongle_engine_start(void)
{
    if (s_mux) return;   /* déjà démarré */
    s_mux = xSemaphoreCreateMutex();
    memset(&s_fusion, 0, sizeof(s_fusion));
    s_dirty = false;

    /* Même init que keyboard_manager_init(), moins le worker de scan local. */
    tap_hold_init();
    tap_dance_init();
    combo_init();
    leader_init();
    key_override_init();
    hid_report_init();

    xTaskCreatePinnedToCore(dongle_engine_task, "dongle_eng", 6144, NULL, 9, NULL, 0);
    ESP_LOGI(TAG, "moteur keymap du dongle démarré (fusion)");
}

#endif /* CONFIG_KASE_DONGLE_FUSION */
