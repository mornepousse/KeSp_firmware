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
#include "fusion_route.h"       /* fusion_dongle_types (règle 3) */
#include "key_processor.h"      /* build_keycode_report, process_matrix_changes, taps */
#include "hid_report.h"         /* send_hid_key */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "key_features.h"       /* key_override_init */
#include "keymap.h"             /* macros_list, MACRO_*, keymaps[], KEYMAP_BLOB_BYTES */
#include "config_sync.h"        /* garde-fou de sync : empreinte + cohérence */
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

/* Mode de la gauche (phase 2). true = la gauche est pilotée par un hôte USB : le
 * dongle se tait (elle tape en local) et réémet la droite → gauche. Drapeau
 * simple : écrit par rf_rx_task (STATUS/MATRIX), lu par la tâche moteur. */
static volatile bool s_left_usb = false;

void dongle_engine_set_left_usb(bool usb)
{
    if (usb != s_left_usb) {
        s_left_usb = usb;
        ESP_LOGW(TAG, "mode gauche : %s", usb ? "USB (dongle se tait, réémet la droite)"
                                              : "sans-fil (dongle tape)");
    }
}

bool dongle_engine_left_usb(void) { return s_left_usb; }

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ── Garde-fou de sync config : la gauche annonce son empreinte, le dongle la
 * compare à la sienne ───────────────────────────────────────────────────────
 *
 * En sans-fil c'est le dongle qui tape, avec SA keymap. Si sa keymap diverge de
 * celle que l'utilisateur a réglée sur la gauche (dongle pas encore
 * reprovisionné, cf. phase 3), il taperait autre chose EN SILENCE. On expose
 * donc la cohérence au contrôleur (CDC) et on la journalise au changement.
 * s_coh : écrit par rf_rx_task (une tâche), lu par le CDC (une autre) — champs
 * u32 alignés, lecture atomique sur Xtensa, valeurs purement diagnostiques. */
static config_coherence_t s_coh;

static inline uint32_t dongle_own_fp(void)
{
    return config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
}

void dongle_engine_note_left_fp(uint32_t fp)
{
    if (!config_coherence_note(&s_coh, fp, now_ms())) return;  /* rien de neuf */
    uint32_t own = dongle_own_fp();
    if (config_fp_match(own, fp))
        ESP_LOGI(TAG, "config cohérente gauche↔dongle : empreinte 0x%08X", (unsigned)fp);
    else
        ESP_LOGW(TAG, "DIVERGENCE de config : gauche=0x%08X dongle=0x%08X — keymaps "
                      "désynchronisées, le dongle tape peut-être autre chose", (unsigned)fp,
                 (unsigned)own);
}

void dongle_engine_get_coherence(uint32_t *own_fp, uint32_t *left_fp,
                                 uint32_t *age_ms, bool *match)
{
    uint32_t own = dongle_own_fp();
    if (own_fp)  *own_fp  = own;
    if (left_fp) *left_fp = s_coh.left_fp;
    if (age_ms)  *age_ms  = s_coh.vue ? (now_ms() - s_coh.left_ms) : 0xFFFFFFFFu;
    if (match)   *match   = config_fp_match(own, s_coh.left_fp);
}

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

        /* Phase 2 : si la gauche est pilotée par USB, le dongle SE TAIT (c'est le
         * moteur de la gauche qui tape). On relâche ce qu'on tenait chez l'hôte à
         * la transition, puis on saute le cycle. drain_radio continue de réémettre
         * la droite → gauche pendant ce temps. */
        static bool prev_types = true;
        bool types = fusion_dongle_types(s_left_usb);
        if (!types) {
            if (prev_types) {
                uint8_t none[6] = {0};
                hid_send_keyboard(0, none);   /* relâche les touches du dongle */
            }
            prev_types = false;
            continue;
        }
        prev_types = true;

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
