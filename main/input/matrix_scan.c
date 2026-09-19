#include "matrix_scan.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* veto TEST */
#endif
#include "matrix_flag.h"

#include "keyboard_task.h"
#include "key_stats.h"
#include "keyboard_config.h"
#include "wake_grace.h"         /* wake_grace_ms — grâce du pilote au réveil (tous boards) */
#include "cdc_binary_protocol.h"
#if CONFIG_KASE_HALF_LINK_TX
#include "half_link.h"
#include "rf_packet.h"
#endif
#if CONFIG_KASE_DONGLE_FUSION
#include "rf_packet.h"          /* rf_matrix_to_bitmap, RF_HALF_LEFT */
#include "fusion_route.h"       /* fusion_left_emits_raw (règle 3) */
#include "half_link.h"          /* half_col_to_keymap (fusion distante 4b) */
#if CONFIG_KASE_KBD_WIRELESS
#include "kbd_relay_tx.h"       /* kbd_relay_send_matrix + remote_pressed/changed */
#include "usb_presence.h"       /* kbd_active_route / KBD_OUT_USB */
#endif
#endif
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
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

#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
/* Sens de rangement des colonnes distantes — propriété du câblage, déclarée par
 * le board.h du maître. 0 = simple décalage, 1 = miroir. */
#ifndef BOARD_REMOTE_COLS_MIRRORED
#define BOARD_REMOTE_COLS_MIRRORED 0
#endif

/* Source de la demi-matrice distante selon le mode :
 *  - fusion, gauche en USB → kbd_relay_remote_pressed (droite réémise par le
 *    dongle et reçue en écoute USB). */
#define KASE_REMOTE_PRESSED(r, c) kbd_relay_remote_pressed((r), (c))

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
            if (KASE_REMOTE_PRESSED(r, c)) {
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
/* État de la matrice capturé au réveil de veille, par balayage manuel avant
 * que le pilote ne soit recréé. matrix_setup() en fait le prev_matrix_state
 * initial du nouveau pilote, puis l'efface : un démarrage ordinaire repart de
 * « rien d'enfoncé » comme avant. */
static uint8_t s_wake_state[MATRIX_ROWS][MATRIX_COLS];
/* Le pilote recréé a-t-il émis un événement ? Il ne signale que les
 * CHANGEMENTS par rapport à son propre état, qui part de « rien d'enfoncé » :
 * une touche capturée au réveil puis relâchée avant son premier balayage ne
 * produit donc AUCUN événement, ni appui ni relâchement. Ce drapeau permet de
 * le savoir. */
static volatile bool s_cb_since_setup;
static bool s_wake_had_keys;


static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle, keyboard_btn_report_t kbd_report, void *user_data)
{
    s_cb_since_setup = true;
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
            rf_matrix_to_bitmap(&new_state[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
            half_link_tx_update(bm, true);
        }
    }
#endif

#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    /* Fusion, côté GAUCHE : cette moitié ne fait plus tourner le moteur ni
     * n'envoie de HID fini (le dongle s'en charge) — elle émet sa demi-matrice
     * BRUTE au dongle, sur changement, comme la droite le fait vers le dongle.
     * KBD_WIRELESS identifie la gauche (la droite est NIPHAR_SLAVE et gagnera son
     * propre chemin au banc). Le miroir n'est PAS appliqué ici : chaque moitié
     * émet ses coordonnées physiques, le dongle range (half_col_to_keymap). */
    {
        bool change = false;
        for (int r = 0; r < MATRIX_ROWS && !change; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                if (new_state[r][c] != prev_matrix_state[r][c]) { change = true; break; }
        /* Règle 3 : on n'émet le brut au dongle QUE hors mode USB. Si un hôte USB
         * est branché à la gauche, c'est SON moteur qui tape en local — alimenter
         * le dongle en plus ferait taper deux fois. Au repos/batterie
         * (route ≠ USB), on émet, le dongle fusionne et tape. */
        bool usb = (kbd_active_route() == KBD_OUT_USB);
        if (change && fusion_left_emits_raw(usb)) {
            uint8_t bm[RF_HALF_BITMAP_BYTES];
            rf_matrix_to_bitmap(&new_state[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
            kbd_relay_send_matrix(RF_HALF_LEFT, bm);
        }
        /* Hors USB, le moteur LOCAL ne sert à rien : c'est le dongle qui tape.
         * Le faire tourner quand même (rapport, tap-hold, combos, HID muet)
         * coûtait du temps par frappe pour un résultat jeté. On s'arrête ici :
         * état mémorisé, activité notée, rien d'autre. USB rebranché → la route
         * bascule et le moteur reprend au balayage suivant (« ne charger le
         * keymap local qu'avec l'USB », 2026-09-16 — c'est l'exécution qu'on
         * conditionne, le code reste là). Le mode test matrice garde la main. */
        if (!fusion_left_types_local(usb) && !matrix_test_mode) {
            memcpy(prev_matrix_state, new_state, sizeof(prev_matrix_state));
            last_activity_time_ms = esp_timer_get_time() / 1000;
            return;
        }
    }
#endif

    /* ── Test mode: send change events, skip HID ── */
    if (matrix_test_mode) {
        uint32_t now = esp_timer_get_time() / 1000;
        /* Auto-exit if no CDC activity or USB disconnected for > 30s */
        if (now - matrix_test_last_activity_ms > MATRIX_TEST_TIMEOUT_MS) {
            matrix_test_mode = false;
#if CONFIG_KASE_VEILLE
            veille_veto(VEILLE_VETO_TEST, false);
#endif
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
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
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

/* Le pilote en économie d'énergie (enable_power_save) POSE un gpio_hold_en sur
 * les colonnes au repos et ne le lève qu'en tête de son propre balayage ;
 * keyboard_button_delete (gpio_reset_pin) ne le lève PAS. Tout ce qui pilote
 * les colonnes hors du pilote — capture au réveil, armement de la veille,
 * recréation — doit d'abord les libérer, sinon les sept colonnes restent
 * hautes ensemble et une touche tenue se lit sur toute sa rangée. */
static void matrix_cols_unhold(void)
{
    const int cols[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                         COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    for (int c = 0; c < MATRIX_COLS; c++) gpio_hold_dis(cols[c]);
}

void rtc_matrix_deinit(void)
{
    ESP_LOGD(TAG, "rtc_matrix_deinit");
    if (s_kbd) {
        keyboard_button_delete(s_kbd);
        s_kbd = NULL;
    }
    matrix_cols_unhold();
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
    /* ⚠ matrix_setup() est sur le CHEMIN DE RÉVEIL. Chaque ligne de journal
     * coûte ~3,5 ms à 115 200 bauds, et la table de brochage en faisait
     * quatorze : le premier balayage n'avait lieu que 60 ms après le réveil.
     * Une frappe brève était déjà relâchée — la touche qui réveillait la
     * carte était perdue. Constaté au banc le 2026-09-11.
     *
     * La table est utile au bring-up, pas à chaque réveil : elle passe en
     * ESP_LOGD, une seule ligne reste en INFO. Un doute sur le brochage se lève
     * avec le niveau de log, pas en ralentissant chaque réveil. */
    ESP_LOGI(TAG, "matrix_setup");
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    memset(SLAVE_MATRIX_STATE, 0, sizeof(SLAVE_MATRIX_STATE));
    /* Le pilote est (re)créé : l'état précédent n'a plus de sens. Sans cet
     * effacement, le premier balayage après un réveil de sommeil léger se
     * compare à l'état d'AVANT le sommeil et fabrique des appuis et des
     * relâchements fantômes. Le repartir de « rien d'enfoncé » fait au contraire
     * que la touche qui a réveillé la carte est vue comme un appui neuf, ce qui
     * est exactement ce qu'on veut. */
    s_cb_since_setup = false;
    memcpy(prev_matrix_state, s_wake_state, sizeof(prev_matrix_state));
    memset(s_wake_state, 0, sizeof(s_wake_state));
    /* Si matrix_wake_capture() vient de publier une touche, le premier
     * balayage du pilote la trouve déjà dans prev : encore tenue → rien de
     * neuf, pas de doublon ; relâchée → un relâchement, et l'hôte la lâche.
     * Sans cela, une touche capturée puis relâchée avant ce balayage resterait
     * COLLÉE chez l'hôte — le pilote n'aurait jamais vu ni l'appui ni le
     * relâchement. */

    // Build gpio arrays from keyboard_config defines
    static int output_gpios[MATRIX_COLS];
    static int input_gpios[MATRIX_ROWS];
#if defined(BOARD_MATRIX_COL2ROW)
    /* Unsized on purpose — see matrix_arm_key_wake() above. */
    const int cols_map[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };

    /* Reset all matrix GPIOs to detach any function set by ROM bootloader
       (UART0 on GPIO43/44, SPI on GPIO37, etc.) */
    matrix_cols_unhold();   /* un maintien survivrait au gpio_reset_pin */
    for (int i = 0; i < MATRIX_COLS; i++) gpio_reset_pin(cols_map[i]);
    for (int i = 0; i < MATRIX_ROWS; i++) gpio_reset_pin(rows_map[i]);
#else
    const int cols_map[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows_map[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
#endif
    ESP_LOGD(TAG, "Cols (outputs): ");
    for (int i = 0; i < MATRIX_COLS; i++) {
        output_gpios[i] = cols_map[i];
        ESP_LOGD(TAG, "  COL%d = GPIO%d", i, output_gpios[i]);
    }
    ESP_LOGD(TAG, "Rows (inputs): ");
    for (int i = 0; i < MATRIX_ROWS; i++) {
        input_gpios[i] = rows_map[i];
        ESP_LOGD(TAG, "  ROW%d = GPIO%d", i, input_gpios[i]);
    }

    keyboard_btn_config_t cfg = {0};
    cfg.output_gpios = output_gpios;
    cfg.input_gpios = input_gpios;
    cfg.output_gpio_num = MATRIX_COLS;
    cfg.input_gpio_num = MATRIX_ROWS;
    cfg.active_level = 1; // Active HIGH 
    cfg.debounce_ticks = BOARD_DEBOUNCE_TICKS;
    cfg.ticks_interval = BOARD_MATRIX_SCAN_INTERVAL_US;
    /* Économie d'énergie du pilote : sans touche enfoncée, le gptimer de 1 ms
     * s'ARRÊTE, les colonnes sont tenues hautes et une interruption sur les
     * lignes le relance au premier appui. Sans cela le processeur sortait
     * d'oisiveté 1000 fois par seconde au repos — chaque fois en rallumant la
     * PLL pour 160 MHz — et le DFS (power/pm_dfs.c) ne descendait jamais
     * vraiment. Le premier balayage suit l'appui en < 1 ms, comme avant. */
    cfg.enable_power_save = true;
    cfg.priority = 5;
    cfg.core_id = 0;

    esp_err_t res = keyboard_button_create(&cfg, &s_kbd);
    if (res == ESP_OK && s_kbd != NULL) {
        ESP_LOGD(TAG, "keyboard_button created: handle=%p", s_kbd);
        
        // KBD_EVENT_PRESSED is called for all changes (press AND release)
        keyboard_btn_cb_config_t cb_pressed = {0};
        cb_pressed.event = KBD_EVENT_PRESSED;
        cb_pressed.callback = keyboard_btn_cb;
        cb_pressed.user_data = NULL;
        esp_err_t r1 = keyboard_button_register_cb(s_kbd, cb_pressed, NULL);
        
        if (r1 == ESP_OK) {
            ESP_LOGD(TAG, "keyboard_button callback registered");
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

/* Un réveil EST une activité. Sans ce coup de tampon, la boucle clavier relit
 * une inactivité de 60 s dès le tour suivant — le pilote recréé n'a pas encore
 * balayé — et renvoie la carte dormir 10 ms après son réveil. La touche encore
 * enfoncée la réveille aussitôt, et le cycle recommence, 80 ms par tour,
 * jusqu'à ce qu'un balayage tombe dans la fenêtre. Constaté au banc le
 * 2026-09-11 : « très lent avant de pouvoir taper », et frappe perdue si on
 * relâche trop tôt. v2d_sleep.c faisait ce geste, il avait été perdu. */
/* Capturer la touche qui a réveillé la carte — AVANT de recréer le pilote.
 *
 * Le réveil GPIO n'a lieu que parce qu'une touche est enfoncée À CET INSTANT :
 * c'est l'information la plus sûre qu'on aura. Or recréer le pilote, attendre
 * son premier balayage et son anti-rebond, rallumer la radio, prend ~90 ms —
 * une frappe brève est relâchée avant, et la touche qui a réveillé le clavier
 * était PERDUE. Constaté au banc le 2026-09-11, et retirer les journaux du
 * chemin n'avait pas suffi.
 *
 * Ici on balaie une fois à la main, en quelques dizaines de microsecondes, et
 * on publie exactement ce que le callback aurait publié. La durée du chemin
 * de réveil cesse d'avoir de l'importance.
 *
 * Préconditions : configuration de réveil en place (matrix_arm_key_wake) —
 * colonnes en sortie, lignes en entrée avec rappel bas. On baisse toutes les
 * colonnes, on les remonte une à une, on lit les lignes, on restaure. Même
 * chaîne électrique que le scan : COL → interrupteur → diode → ROW. */
void matrix_wake_capture(void)
{
    const int cols[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                         COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
    const int rows[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };
    /* DEUX balayages, on ne garde que ce qui tient sur les deux. Le réveil GPIO
     * se déclenche sur un simple front : une ligne qui glitche (couplage
     * capacitif des colonnes voisines tenues hautes, ESD, ligne au seuil)
     * réveille la carte et se lit « pressée » sur un balayage unique. Elle
     * ne survit pas à deux lectures espacées de 1 ms ; un vrai appui, si.
     *
     * Sans ce filtre, chaque réveil fantôme (cause=7, ~3 par 10 min mesurés le
     * 2026-09-12) faisait deux dégâts : il tamponnait 60 s d'activité — radio
     * allumée, 0,2 V perdus sur une nuit — ET la capture PUBLIAIT la touche,
     * qui partait taper un caractère parasite vers le dongle. */
    uint8_t st[MATRIX_ROWS][MATRIX_COLS];
    uint8_t st2[MATRIX_ROWS][MATRIX_COLS];
    memset(st, 0, sizeof(st));
    memset(st2, 0, sizeof(st2));
    matrix_cols_unhold();   /* le pilote détruit a pu laisser ses maintiens */

    for (int pass = 0; pass < 2; pass++) {
        uint8_t (*dst)[MATRIX_COLS] = (pass == 0) ? st : st2;
        for (int c = 0; c < MATRIX_COLS; c++) gpio_set_level(cols[c], 0);
        for (int c = 0; c < MATRIX_COLS; c++) {
            gpio_set_level(cols[c], 1);
            esp_rom_delay_us(20);             /* RC des 100 Ω série + capacité */
            for (int r = 0; r < MATRIX_ROWS; r++)
                dst[r][c] = (uint8_t)gpio_get_level(rows[r]);
            gpio_set_level(cols[c], 0);
        }
        if (pass == 0) esp_rom_delay_us(1000);   /* 1 ms entre les deux passes */
    }
    for (int c = 0; c < MATRIX_COLS; c++) gpio_set_level(cols[c], 1);

    /* ET des deux passes : un fantôme transitoire tombe, un vrai appui reste. */
    uint8_t st1[MATRIX_ROWS][MATRIX_COLS];
    memcpy(st1, st, sizeof(st1));   /* passe 1 brute, pour le diagnostic ci-dessous */
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            st[r][c] = st[r][c] && st2[r][c];

    /* Publier comme le callback : rapport local, frontière, fusion, drapeau. */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i]  = INVALID_KEY_POS;
        current_press_col[i]  = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
    uint8_t filled = 0;
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS && filled < MAX_REPORT_KEYS; c++)
            if (st[r][c]) {
                current_press_row[filled]  = (uint8_t)r;
                current_press_col[filled]  = (uint8_t)c;
                current_press_stat[filled] = 1;
                filled++;
            }
    memcpy(MATRIX_STATE, st, sizeof(MATRIX_STATE));
    memcpy(s_wake_state, st, sizeof(s_wake_state));   /* pour matrix_setup */
    s_wake_had_keys = (filled != 0);
    memcpy(prev_matrix_state, st, sizeof(prev_matrix_state));
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    s_filled_local = filled;
    matrix_apply_remote();
#endif
#if CONFIG_KASE_HALF_LINK_TX
    /* La moitié droite n'a pas de tâche clavier : c'est le callback qui émet.
     * On émet donc ici ce qu'il aurait émis. */
    if (filled) {   /* réveil fantôme (rien d'enfoncé) : muet, rien à annoncer */
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        rf_matrix_to_bitmap(&st[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
        half_link_tx_update(bm, true);
    }
#endif
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    /* Fusion, côté GAUCHE : le même trou que la droite comble juste au-dessus.
     * Le seul émetteur brut de la gauche est le callback sur CHANGEMENT — or la
     * capture pose prev_matrix_state = st, donc le scanner recréé voit la touche
     * tenue SANS changement et n'émet rien ; seul le relâchement partait. La
     * première touche après le light sleep était avalée (banc 2026-09-13 ; la
     * séquence de veille.c émettait sur le chemin pré-fusion (retiré le 2026-09-18),
     * compilé out ici). On émet donc l'appui capturé tout de suite, hors USB
     * (règle 3), avec le même émetteur que le callback — la réaffirmation à
     * 100 ms prend ensuite le relais tant que la touche est tenue. */
    if (filled && fusion_left_emits_raw(kbd_active_route() == KBD_OUT_USB)) {
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        rf_matrix_to_bitmap(&st[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
        kbd_relay_send_matrix(RF_HALF_LEFT, bm);
    }
#endif
    if (filled) {
        matrix_flag_signal(&stat_matrix_changed);
        if (keyboard_task_handle != NULL) xTaskNotifyGive(keyboard_task_handle);
        /* Une TOUCHE est une activité. Un réveil sans touche — glitch sur une
         * ligne, couplage du câble TRRS, bruit — ne l'est PAS : le tamponner
         * achetait 60 s de radio allumée à chaque parasite. Une nuit du
         * 2026-09-12 : 1,2 h d'éveil sur 7 h pour ~70 réveils fantômes, 0,2 V
         * perdus. Sans tampon, la boucle relit une inactivité ancienne et
         * renvoie dormir en ~15 ms — exactement ce qu'un glitch mérite. Une
         * vraie touche que la capture aurait manquée serait vue par le pilote
         * dans ces 15 ms et tamponnerait par le callback. */
        last_activity_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    /* Une ligne par réveil : ce que la capture a trouvé. C'est elle qui a
     * prouvé, le 2026-09-11, que la gauche voyait bien la touche de réveil. */
    ESP_LOGI(TAG, "reveil : %u touche(s) capturee(s)", filled);
    if (filled == 0) {
        /* Capture vide sur un réveil GPIO : dire ce que CHAQUE passe a lu, pour
         * distinguer un rebond (passe 1 pleine, passe 2 vide ou l'inverse) d'un
         * pré-contact ou d'un fantôme (les deux vides). Banc 2026-09-16 :
         * « touche de réveil perdue sur la gauche, depuis toujours ». */
        char l1[MATRIX_ROWS * MATRIX_COLS + 1], l2[MATRIX_ROWS * MATRIX_COLS + 1];
        int k = 0;
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++, k++) { l1[k] = st1[r][c] ? '1' : '.'; l2[k] = st2[r][c] ? '1' : '.'; }
        l1[k] = l2[k] = '\0';
        ESP_LOGW(TAG, "  capture vide : passe1=%s passe2=%s", l1, l2);
    }
    for (uint8_t i = 0; i < filled; i++)
        ESP_LOGI(TAG, "  (%u,%u)", current_press_row[i], current_press_col[i]);
}

/* À appeler ~10 ms après matrix_setup(), quand le pilote a eu le temps de
 * faire son premier balayage et son anti-rebond.
 *
 * Si la capture au réveil avait trouvé une touche et que le pilote n'a RIEN
 * signalé depuis, c'est que la touche a été relâchée entre les deux : le
 * pilote, parti de « rien d'enfoncé », a vu « rien d'enfoncé » et s'est tu.
 * Sans cette réconciliation la touche restait dans le rapport jusqu'au
 * prochain événement de CETTE moitié — les frappes de l'autre moitié ne la
 * délogent pas, la fusion préserve les touches locales. Un pouce Super
 * capturé au réveil transformait alors toute la frappe de droite en
 * raccourcis. Constaté au banc le 2026-09-11.
 *
 * Retourne true si un relâchement a été publié : l'appelant doit alors
 * l'émettre. */
bool matrix_wake_had_keys(void) { return s_wake_had_keys; }

void matrix_wake_wait_first_scan(void)
{
    /* Attente CONDITIONNELLE, pas un délai deviné : on sort dès que le pilote a
     * rappelé (touche tenue confirmée — la capture l'a déjà émise, rien à faire),
     * sinon on attend la grâce déduite de son anti-rebond avant de conclure au
     * relâchement. vTaskDelay(1) dans la boucle rend la main au pilote (tâche
     * prio 5) sans jamais l'affamer ; le temps, lui, se mesure à esp_timer. */
    const uint32_t grace = wake_grace_ms(BOARD_DEBOUNCE_TICKS, BOARD_MATRIX_SCAN_INTERVAL_US);
    const uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (!s_cb_since_setup &&
           (uint32_t)((uint32_t)(esp_timer_get_time() / 1000) - t0) < grace)
        vTaskDelay(1);
}

bool matrix_wake_reconcile(void)
{
    if (!s_wake_had_keys || s_cb_since_setup) return false;
    s_wake_had_keys = false;
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i]  = INVALID_KEY_POS;
        current_press_col[i]  = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    memset(prev_matrix_state, 0, sizeof(prev_matrix_state));
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    s_filled_local = 0;
    matrix_apply_remote();
#endif
#if CONFIG_KASE_HALF_LINK_TX
    {
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        memset(bm, 0, sizeof(bm));
        half_link_tx_update(bm, true);
    }
#endif
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    /* Fusion, gauche : la touche de réveil a été émise à la capture ; relâchée
     * avant le premier balayage, le callback ne le dira jamais — on émet le
     * relâchement, sinon elle reste collée au dongle. */
    if (fusion_left_emits_raw(kbd_active_route() == KBD_OUT_USB)) {
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        memset(bm, 0, sizeof(bm));
        kbd_relay_send_matrix(RF_HALF_LEFT, bm);
    }
#endif
    matrix_flag_signal(&stat_matrix_changed);
    ESP_LOGW(TAG, "reveil : touche relachee avant le premier balayage, relachement publie");
    return true;
}

void matrix_mark_activity(void)
{
    last_activity_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

