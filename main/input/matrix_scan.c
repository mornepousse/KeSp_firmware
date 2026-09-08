#include "matrix_scan.h"
#include "matrix_flag.h"

#include "keyboard_task.h"
#include "key_stats.h"
#include "keyboard_config.h"
#include "cdc_binary_protocol.h"
#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
#include "half_link.h"
#include "rf_packet.h"
#endif
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_KASE_HAS_DISPLAY
#include "status_display.h"
#endif
#include "driver/gpio.h"
#include "esp_sleep.h"

#define TAG "MATRIX_SCAN"

/* ── Matrix test mode ──────────────────────────────────────────── */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
#define MATRIX_TEST_TIMEOUT_MS 30000  /* 30s auto-exit */

// Define variables expected by other code
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS] = { &MATRIX_STATE, &SLAVE_MATRIX_STATE };
#define MAX_REPORT_KEYS  6       /* HID boot protocol: max 6 simultaneous keys */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];

#if CONFIG_KASE_HALF_LINK_RX
/* Sens de rangement des colonnes distantes — propriété du câblage, déclarée par
 * le board.h du maître. 0 = simple décalage, 1 = miroir. */
#ifndef BOARD_REMOTE_COLS_MIRRORED
#define BOARD_REMOTE_COLS_MIRRORED 0
#endif

/* Frontière entre les entrées du balayage LOCAL et celles reçues par radio.
 * Tout ce qui est au-delà appartient à la moitié distante et se reconstruit à
 * chaque appel de matrix_apply_remote(). */
static uint8_t s_filled_local;

/* Fusion des deux moitiés — IDEMPOTENTE, donc appelable à chaque cycle.
 *
 * Elle doit l'être : le callback de scan ne tourne que sur activité LOCALE, or
 * un appui sur la seule moitié droite n'en produit aucune. Sans un appel
 * périodique depuis la tâche clavier, les touches reçues par radio
 * n'atteindraient jamais le moteur — c'est le défaut qui faisait que la droite
 * ne tapait rien alors que son lien était acquitté.
 *
 * Les touches distantes occupent les colonnes 7-13, cette moitié les 0-6. Le
 * moteur ne sait pas d'où elles viennent, il indexe keymaps[layer][row][col] et
 * rien d'autre. Le SENS de rangement dépend du câblage et passe par
 * half_col_to_keymap() : les deux moitiés étant le même PCB retourné, la
 * colonne 0 de la droite est sa touche la plus à droite.
 *
 * Le plafond MAX_REPORT_KEYS est respecté, et les touches locales gardent la
 * priorité puisqu'elles occupent le début du tableau. */
void matrix_apply_remote(void)
{
    uint8_t filled = s_filled_local;
    for (uint8_t i = filled; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i]  = INVALID_KEY_POS;
        current_press_col[i]  = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
    for (uint8_t r = 0; r < MATRIX_ROWS && filled < MAX_REPORT_KEYS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS && filled < MAX_REPORT_KEYS; c++)
            if (half_link_remote_pressed(r, c)) {
                current_press_row[filled]  = r;
                current_press_col[filled]  = half_col_to_keymap(
                        c, MATRIX_COLS, BOARD_REMOTE_COLS_MIRRORED);
                current_press_stat[filled] = 1;
                filled++;
            }
}
#endif
volatile uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
uint8_t current_layout = 0;
volatile uint8_t is_layer_changed = 0;
volatile uint32_t last_activity_time_ms = 0;

static keyboard_btn_handle_t s_kbd = NULL;
static uint8_t prev_matrix_state[MATRIX_ROWS][MATRIX_COLS];  /* For KPM: track new keypresses */


static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle, keyboard_btn_report_t kbd_report, void *user_data)
{
    /* Build current matrix state from report */
    uint8_t new_state[MATRIX_ROWS][MATRIX_COLS];
    memset(new_state, 0, sizeof(new_state));
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS)
                new_state[in_idx][out_idx] = 1;
        }
    }

#if CONFIG_KASE_MATRIX_LOG_CONSOLE
    /* Journal de banc : chaque changement en clair sur la console, avant que
     * prev_matrix_state ne soit ecrase par l'un ou l'autre des chemins. Ne
     * depend pas du CDC, contrairement au mode test ci-dessous. */
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            if (new_state[r][c] != prev_matrix_state[r][c])
                ESP_LOGW(TAG, "MTX r=%d c=%d %s", r, c,
                         new_state[r][c] ? "appui" : "relache");
#endif

#if CONFIG_KASE_HALF_LINK_TX
    /* Moitié droite : émettre la demi-matrice à chaque changement. Événementiel
     * et non périodique — c'est la prémisse §2.3 du design, et la seule qui
     * rende le pari R1 tenable. Contexte tâche (le callback vient du pilote
     * keyboard_button, pas d'une ISR), donc rf_driver_send peut y bloquer le
     * temps de ses retransmissions. */
    {
        bool change = false;
        for (int r = 0; r < MATRIX_ROWS && !change; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                if (new_state[r][c] != prev_matrix_state[r][c]) { change = true; break; }
        if (change) {
            uint8_t bm[RF_HALF_BITMAP_BYTES];
            memset(bm, 0, sizeof(bm));
            for (int r = 0; r < MATRIX_ROWS; r++)
                for (int c = 0; c < MATRIX_COLS; c++)
                    if (new_state[r][c]) rf_bitmap_set(bm, (uint8_t)r, (uint8_t)c, true);
            half_link_tx_update(bm, true);
        }
    }
#endif

    /* ── Test mode: send change events, skip HID ── */
    if (matrix_test_mode) {
        uint32_t now = esp_timer_get_time() / 1000;
        /* Auto-exit if no CDC activity or USB disconnected for > 30s */
        if (now - matrix_test_last_activity_ms > MATRIX_TEST_TIMEOUT_MS) {
            matrix_test_mode = false;
            ESP_LOGW(TAG, "matrix test mode timeout — auto-exit");
        } else {
            for (int r = 0; r < MATRIX_ROWS; r++) {
                for (int c = 0; c < MATRIX_COLS; c++) {
                    if (new_state[r][c] != prev_matrix_state[r][c]) {
                        uint8_t evt[3] = { (uint8_t)r, (uint8_t)c, new_state[r][c] };
                        matrix_test_last_activity_ms = now;
                        ks_respond(KS_CMD_MATRIX_TEST, KS_STATUS_OK, evt, 3);
                    }
                }
            }
            memcpy(prev_matrix_state, new_state, sizeof(prev_matrix_state));
            last_activity_time_ms = now;
            return;
        }
        /* Fallthrough to normal mode if just auto-exited */
    }

    /* ── Normal mode ── */
    memcpy(MATRIX_STATE, new_state, sizeof(MATRIX_STATE));

    /* ⚠ NE PAS remettre keycodes[] à zéro ici. Ce tableau appartient au
     * producteur de rapport : build_keycode_report() parcourt les six
     * emplacements et écrit 0 dans chacun de ceux qui sont vides
     * (key_processor.c, « Step 4 »), donc il le détermine entièrement — l'effacer
     * ici n'apportait rien.
     *
     * Mais ce callback tourne dans la tâche du pilote keyboard_button, en
     * priorité 5, pendant que vTaskKeyboard est peut-être ENTRE
     * build_keycode_report() et send_hid_key() — une fenêtre qui contient tout
     * process_matrix_changes(). L'effacement partait alors juste avant l'envoi :
     * le rapport sortait VIDE, et le cycle suivant reconstruisait à partir d'un
     * état où la touche était déjà relâchée. L'appui n'était jamais transmis.
     *
     * Constaté au banc le 2026-09-08 : en frappe rapide sur la moitié gauche,
     * une touche sautait. Le mode test matrice (KS_CMD_MATRIX_TEST) a montré
     * 45 événements sans le moindre trou — le balayage voyait tout, la perte
     * était ici. */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }

    uint8_t filled = 0;
    uint8_t new_keypresses = 0;
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num && filled < MAX_REPORT_KEYS; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS) {
                if (!prev_matrix_state[in_idx][out_idx]) {
                    new_keypresses++;
                    key_stats_record_press(in_idx, out_idx);
                }
                current_press_row[filled] = in_idx;
                current_press_col[filled] = out_idx;
                current_press_stat[filled] = 1;
                filled++;
            }
        }
    }

    /* ⚠ Le #if entoure la boucle ENTIÈRE, accolades comprises. Ne l'appliquer
     * qu'au corps d'un `for` sans accolades ferait de l'instruction suivante ce
     * corps — ici le memcpy de prev_matrix_state, qui cesserait alors d'être
     * exécuté dès qu'un cycle ne compte aucun nouvel appui. Observé au banc sur
     * la moitié droite : chaque relâchement était signalé deux fois.
     *
     * La moitié droite du Niphargus scanne sans écran (module non compilé) : le
     * lien échouait sur ce seul symbole. Même cas que v2d_sleep.c. */
    /* ⚠ LA FUSION N'A RIEN À VOIR AVEC L'ÉCRAN — ne jamais la remettre sous
     * CONFIG_KASE_HAS_DISPLAY. Elle y a été imbriquée par accident le
     * 2026-09-07, en corrigeant le piège du `for` sans accolades ci-dessus, et
     * la moitié gauche n'ayant pas d'écran, ces deux lignes n'étaient PAS
     * compilées : s_filled_local restait à zéro, donc matrix_apply_remote()
     * repartait de l'indice 0 et ÉCRASAIT les touches locales, tandis que le
     * chemin local n'appelait jamais la fusion et effaçait les distantes.
     * Chaque moitié tapait seule, et AUCUNE combinaison entre les deux ne
     * passait — Maj à gauche + lettre à droite, notamment. */
#if CONFIG_KASE_HALF_LINK_RX
    s_filled_local = filled;   /* frontiere local / distant, pour la fusion */
    matrix_apply_remote();
#endif

#if CONFIG_KASE_HAS_DISPLAY
    for (uint8_t k = 0; k < new_keypresses; k++)
        status_display_notify_keypress();
#else
    (void)new_keypresses;
#endif

    memcpy(prev_matrix_state, new_state, sizeof(prev_matrix_state));
    matrix_flag_signal(&stat_matrix_changed);
    last_activity_time_ms = esp_timer_get_time() / 1000;

    if (keyboard_task_handle != NULL)
        xTaskNotifyGive(keyboard_task_handle);
}

void rtc_matrix_deinit(void)
{
    ESP_LOGI(TAG, "rtc_matrix_deinit (shim)");
    if (s_kbd) {
        keyboard_button_delete(s_kbd);
        s_kbd = NULL;
    }
}

/* ── Light-sleep matrix key-wake (V2D wireless) ──────────────────────────────
 * The scan driver drives COLS (outputs) and reads ROWS (inputs). For a static
 * wake, drive every COL HIGH and arm each ROW as a HIGH-level wake source: a
 * keypress ties a HIGH col to its row → row goes HIGH → wakes light-sleep.
 * gpio_sleep_sel_dis keeps this config live during sleep (else IDF isolates the
 * pads and no key can wake). Call rtc_matrix_deinit() first (driver released).
 * BENCH-TUNE: active level is HIGH here; flip if the matrix is active-low. */
void matrix_arm_key_wake(void)
{
    /* Sized by the initializer, not by MATRIX_COLS/MATRIX_ROWS: boards with
     * fewer than 13 cols / 5 rows (e.g. Niphargus 7x4) pad the tail with
     * GPIO_NUM_NC in board.h. The loops below still stop at MATRIX_COLS/
     * MATRIX_ROWS, so those padding entries are declared but never read —
     * an explicit [MATRIX_COLS]/[MATRIX_ROWS] size here would make the
     * literal initializer overflow and warn "excess elements" on every such
     * board. See main/input/matrix_scan.c module comment. */
    const int cols[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                                    COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };

    for (int i = 0; i < MATRIX_COLS; i++) {
        gpio_set_direction(cols[i], GPIO_MODE_OUTPUT);
        gpio_set_level(cols[i], 1);
        gpio_sleep_sel_dis(cols[i]);
    }
    for (int i = 0; i < MATRIX_ROWS; i++) {
        gpio_set_direction(rows[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(rows[i], GPIO_PULLDOWN_ONLY);
        gpio_wakeup_enable(rows[i], GPIO_INTR_HIGH_LEVEL);
        gpio_sleep_sel_dis(rows[i]);
    }
    esp_sleep_enable_gpio_wakeup();
}

void matrix_disarm_key_wake(void)
{
    /* See matrix_arm_key_wake(): unsized on purpose, loop bound is real. */
    const int rows[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
    for (int i = 0; i < MATRIX_ROWS; i++) {
        gpio_wakeup_disable(rows[i]);
    }
    /* matrix_setup() re-takes the pins for the scan driver. */
}

void matrix_setup(void)
{
    ESP_LOGI(TAG, "matrix_setup (shim)");
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    memset(SLAVE_MATRIX_STATE, 0, sizeof(SLAVE_MATRIX_STATE));

    // Build gpio arrays from keyboard_config defines
    static int output_gpios[MATRIX_COLS];
    static int input_gpios[MATRIX_ROWS];
#if defined(BOARD_MATRIX_COL2ROW)
    /* Unsized on purpose — see matrix_arm_key_wake() above. */
    const int cols_map[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };

    /* Reset all matrix GPIOs to detach any function set by ROM bootloader
       (UART0 on GPIO43/44, SPI on GPIO37, etc.) */
    for (int i = 0; i < MATRIX_COLS; i++) gpio_reset_pin(cols_map[i]);
    for (int i = 0; i < MATRIX_ROWS; i++) gpio_reset_pin(rows_map[i]);
#else
    const int cols_map[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
#endif
    ESP_LOGI(TAG, "Cols (outputs): ");
    for (int i = 0; i < MATRIX_COLS; i++) {
        output_gpios[i] = cols_map[i];
        ESP_LOGI(TAG, "  COL%d = GPIO%d", i, output_gpios[i]);
    }
    ESP_LOGI(TAG, "Rows (inputs): ");
    for (int i = 0; i < MATRIX_ROWS; i++) {
        input_gpios[i] = rows_map[i];
        ESP_LOGI(TAG, "  ROW%d = GPIO%d", i, input_gpios[i]);
    }

    keyboard_btn_config_t cfg = {0};
    cfg.output_gpios = output_gpios;
    cfg.input_gpios = input_gpios;
    cfg.output_gpio_num = MATRIX_COLS;
    cfg.input_gpio_num = MATRIX_ROWS;
    cfg.active_level = 1; // Active HIGH 
    cfg.debounce_ticks = BOARD_DEBOUNCE_TICKS;
    cfg.ticks_interval = BOARD_MATRIX_SCAN_INTERVAL_US;
    cfg.enable_power_save = false;
    cfg.priority = 5;
    cfg.core_id = 0;

    esp_err_t res = keyboard_button_create(&cfg, &s_kbd);
    if (res == ESP_OK && s_kbd != NULL) {
        ESP_LOGI(TAG, "keyboard_button created: handle=%p", s_kbd);
        
        // KBD_EVENT_PRESSED is called for all changes (press AND release)
        keyboard_btn_cb_config_t cb_pressed = {0};
        cb_pressed.event = KBD_EVENT_PRESSED;
        cb_pressed.callback = keyboard_btn_cb;
        cb_pressed.user_data = NULL;
        esp_err_t r1 = keyboard_button_register_cb(s_kbd, cb_pressed, NULL);
        
        if (r1 == ESP_OK) {
            ESP_LOGI(TAG, "keyboard_button callback registered");
        } else {
            ESP_LOGW(TAG, "keyboard_button_register_cb failed: %d", r1);
        }
    } else {
        ESP_LOGW(TAG, "keyboard_button_create failed: %d", res);
    }
}

void layer_changed(void)
{
    is_layer_changed = 1;
}

uint32_t get_last_activity_time_ms(void)
{
    return last_activity_time_ms;
}

