/*
 * KeSp — keyboard firmware framework
 */
#include "cdc_acm_com.h"
#include "cpu_time.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#if CONFIG_KASE_LINK_WIRE
#include "link_uart.h"
#endif
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
#endif
#if CONFIG_KASE_DISPLAY_MEMLCD
#include "memlcd_panel.h"   /* memlcd_cs_idle : CS écran bas dès le boot (bus partagé nRF24) */
#endif
#if CONFIG_KASE_VEILLE
#include "veille.h"
#endif
#include "pm_dfs.h"   /* vide sans CONFIG_PM_ENABLE */
#if CONFIG_PM_PROFILING
#include "esp_pm.h"
#endif
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "key_features.h"
#include "key_stats.h"
#include "keymap.h"
#include "nvs_flash.h"
#include "usb_hid.h"
#if CONFIG_KASE_SEC_OPENPGP
#include "esp_mac.h"
#endif
#include <stdint.h>

#if CONFIG_KASE_HAS_DISPLAY
#include "display_backend.h"   /* tout rôle avec un écran (clavier, moitié droite) */
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "hid_bluetooth_manager.h"   /* real API, or no-op stubs when HAS_BLE off */
#include "keyboard_task.h"
#include "led_strip_anim.h"
#include "status_display.h"
#endif

/* matrix_scan.h suit la matrice, pas le rôle : la moitié droite du Niphargus
 * scanne aussi (KASE_HAS_LOCAL_MATRIX) sans être un clavier complet. */
#if CONFIG_KASE_HAS_LOCAL_MATRIX
#include "matrix_scan.h"
#endif

#if CONFIG_KASE_KBD_WIRELESS
#include "kbd_relay_tx.h"
#include "usb_presence.h"   /* kbd_active_route — affiche par le battement de coeur */
#endif

/* Diagnostic de banc, independant du role : la gauche du Niphargus n'a ni
 * KBD_WIRELESS ni HAS_RF_RX, l'include doit donc vivre hors de ces blocs. */
#if CONFIG_KASE_NRF_PROBE
#include "rf_probe.h"
#endif

#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
#include "half_link.h"
#endif

#if CONFIG_KASE_HAS_RF_RX
#include "trackpad.h"
#endif

/* Runtime debug/experimental flags: set to 1 to skip starting the component for
 * isolation testing */
#ifndef SKIP_STATUS_DISPLAY
#define SKIP_STATUS_DISPLAY 0
#endif

static const char *TAG = "Main";

/* Task handle exported for diagnostics: status display task */
TaskHandle_t status_display_task_handle = NULL;

#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
static void cpu_time_logger_task(void *arg) {
  (void)arg;
  char buf[512];
  for (;;) {
    if (cpu_time_measure_period(1000, buf, sizeof(buf)) == 0) {
      ESP_LOGI(TAG, "CPU usage:\n%s", buf);
    } else {
      /* Les statistiques FreeRTOS ne sont pas compilees
       * (CONFIG_FREERTOS_USE_TRACE_FACILITY absent) : ce message tombait toutes
       * les 5 s sans rien apprendre. On en fait un battement de coeur, seul
       * temoin de vie quand la carte tourne sur batterie — l'USB est alors
       * debranche et ne dit plus rien. Il affiche aussi le routage, ce qui
       * permet de verifier que la bascule USB -> RF a bien eu lieu. */
      uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
      uint32_t dodo_n = 0, dodo_ms = 0;
#if CONFIG_KASE_VEILLE
      veille_bilan(&dodo_n, &dodo_ms);   /* « dormi X s sur Y » : lit une nuit d'un coup d'oeil */
#endif
      uint32_t inactif_s = (uint32_t)((esp_timer_get_time() / 1000) - get_last_activity_time_ms()) / 1000;
#if CONFIG_PM_PROFILING
      esp_pm_dump_locks(stdout);   /* banc : temps passé par mode (light sleep, APB min/max) */
#endif
#if CONFIG_KASE_KBD_WIRELESS
      ESP_LOGW(TAG, "HB up=%us inactif=%us dormi=%us/%u route=%s relais=%s", (unsigned)up_s,
               (unsigned)inactif_s, (unsigned)(dodo_ms / 1000), (unsigned)dodo_n,
               (kbd_active_route() == KBD_OUT_RF) ? "RF" : "USB",
               kbd_relay_active() ? "actif" : "inactif");
#else
      ESP_LOGW(TAG, "HB up=%us inactif=%us dormi=%us/%u", (unsigned)up_s, (unsigned)inactif_s,
               (unsigned)(dodo_ms / 1000), (unsigned)dodo_n);
#endif
    }
    /* 10 s : ce battement est un témoin de banc (inactif, dormi, route), pas
     * un service ; à 2 s il coûtait une ligne série et une sortie d'oisiveté
     * toutes les deux secondes. Assez pour lire une nuit. */
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
#endif

#if CONFIG_KASE_HAS_DISPLAY
#if CONFIG_KASE_DEVICE_ROLE_NIPHAR_SLAVE && CONFIG_KASE_DISPLAY_MEMLCD
/* Écran de la DROITE : tâche minimale, voir l'appel dans app_main (rôle
 * esclave). La tâche clavier ci-dessous tire le moteur et les stats, absents. */
static void memlcd_slave_display_task(void *arg) {
  (void)arg;
  const display_backend_t *be = display_get_backend();
  if (!be || !be->init()) { ESP_LOGW(TAG, "ecran droite : init KO"); vTaskDelete(NULL); return; }
  for (;;) { be->update(); vTaskDelay(pdMS_TO_TICKS(100)); }
}
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
// Task handling status display updates, sleep/wake and layer change handling.
static uint8_t last_displayed_layer =
    255; // Track what layer is currently shown
static int display_sleep = 0; // 0 = on, 1 = off

static void status_display_task(void *arg) {
  (void)arg;
  for (;;) {
    // Check both the flag AND if the displayed layer matches current
    // This catches rapid layer changes that might set/clear the flag quickly
    if (is_layer_changed || (last_displayed_layer != current_layout)) {
      is_layer_changed = 0;
      last_displayed_layer = current_layout;
      status_display_update_layer_name();
    }

    /* Display sleep: turn off after inactivity, wake on next keypress */
    uint32_t now = esp_timer_get_time() / 1000;
    uint32_t last = get_last_activity_time_ms();

    if (!display_sleep && last != 0 && (now - last) > BOARD_DISPLAY_SLEEP_MS) {
      status_display_sleep();
      display_sleep = 1;
    }

    if (display_sleep && last != 0 && (now - last) <= 500) {
      status_display_wake();
      display_sleep = 0;
    }

    /* Handle deferred wake requests in display task context */
    if (request_wake_request) {
      request_wake_request = false;
      status_display_refresh_all();
    }

    status_display_update();

    /* Periodically save stats + tick WPM every second */
    key_stats_check_save();   /* no-op sur les moitiés : CONFIG_KASE_KEY_STATS=n */
    {
      static uint32_t last_wpm_tick = 0;
      uint32_t now_wpm = esp_timer_get_time() / 1000;
      if (now_wpm - last_wpm_tick > 1000) {
        wpm_tick();
        last_wpm_tick = now_wpm;
      }
    }


    vTaskDelay(pdMS_TO_TICKS(
        100)); // 100ms polling — fast enough for UI, no keyboard lag
  }
}
#endif /* CONFIG_KASE_DEVICE_ROLE_KEYBOARD */
#endif /* CONFIG_KASE_HAS_DISPLAY */

/* Boot crash detection: RTC memory survives soft reboot but not power cycle */
static RTC_NOINIT_ATTR uint32_t boot_crash_count;
static RTC_NOINIT_ATTR uint32_t boot_crash_magic;
#define BOOT_CRASH_MAGIC 0xB007FA11
#define BOOT_CRASH_LIMIT 3

static bool safe_mode = false;

void app_main(void) {
  ESP_LOGI(TAG, "--------------- KeSp [%s] ----------------", PRODUCT_NAME);

  /* Crash-loop detection — RTC memory is random on first power-on,
     so we validate with a magic number AND a reasonable count range */
  if (boot_crash_magic != BOOT_CRASH_MAGIC || boot_crash_count > 100) {
    boot_crash_magic = BOOT_CRASH_MAGIC;
    boot_crash_count = 0;
  }
  boot_crash_count++;
  ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)boot_crash_count);

#if CONFIG_KASE_VEILLE
  /* D'où vient ce démarrage ? Un réveil de sommeil profond est un REDÉMARRAGE :
   * sans cette ligne, il est indiscernable d'une mise sous tension ou d'un
   * plantage, et le journal ne permet pas de dire si EXT1 a fonctionné.
   * Éprouvé au banc le 2026-09-08 : rst:0x5 (DSLEEP), Boot OK à 702 ms. */
  {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1)
      ESP_LOGW(TAG, "reveil EXT1 : une touche a sorti la carte du sommeil profond");
    else if (cause != ESP_SLEEP_WAKEUP_UNDEFINED)
      ESP_LOGW(TAG, "reveil de veille, cause=%d", (int)cause);
  }
  /* AVANT toute configuration de matrice : les colonnes peuvent être encore
   * figées par le maintien RTC posé avant le sommeil profond. */
  veille_liberer_gpio();
#endif
#if CONFIG_KASE_DISPLAY_MEMLCD
  /* Écran sur le SPI PARTAGÉ avec la radio : son CS (actif haut) est tenu BAS
   * dès maintenant, AVANT toute init radio, pour qu'il n'écoute jamais le bus. */
  memlcd_cs_idle();
#endif
#if CONFIG_KASE_BATT_SENSE
  /* Jauge : premiere mesure au boot, puis toutes les 10 s et a chaque reveil. */
  batt_sense_init();
#endif
  /* Frequence dynamique : 160 MHz en travail, 40 MHz oisif (power/pm_dfs.c).
   * Rien sans CONFIG_PM_ENABLE (moities Niphargus seulement). */
  pm_dfs_init();

  if (boot_crash_count > BOOT_CRASH_LIMIT) {
    ESP_LOGW(TAG, "Crash loop detected (%lu boots) — SAFE MODE",
             (unsigned long)boot_crash_count);
    safe_mode = true;
    boot_crash_count = 0;
    /* Don't erase NVS — user data is precious.
       Safe mode just skips display/BLE/NVS loading. */
  }

  /* ── NVS, pour TOUS les rôles ────────────────────────────────────────────
   *
   * Elle n'était initialisée que par keymap_init_nvs(), dans input/keymap.c,
   * derrière la garde du moteur keymap. Ni le dongle ni la souris ne compilent
   * ce fichier — et hid_bluetooth_manager.c, l'autre appelant, pas davantage.
   *
   * Conséquence, constatée au banc le 2026-08-26 : le dongle répondait
   * ESP_ERR_NVS_NOT_INITIALIZED (0x1101) à toute écriture. Il acquittait les
   * demandes d'appairage sans jamais rien enregistrer, et n'avait aucun moyen
   * de le dire — il tourne sans console en production. La souris présentait
   * exactement le même symptôme, pour exactement la même raison.
   *
   * La NVS est de l'infrastructure : elle n'a rien à faire derrière une garde
   * de rôle. keymap_init_nvs() reste appelée plus bas pour les rôles clavier ;
   * un second nvs_flash_init() est sans effet. */
  {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_LOGW(TAG, "NVS a effacer et reinitialiser (%s)", esp_err_to_name(nvs));
      ESP_ERROR_CHECK(nvs_flash_erase());
      nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK)
      ESP_LOGE(TAG, "NVS indisponible : %s — appairage et config ne seront pas "
                    "persistes", esp_err_to_name(nvs));
  }

  kase_tinyusb_init();
  init_cdc_commands();

  /* Register binary CDC protocol handlers */
  {
    extern void cdc_binary_cmds_init(void);
    cdc_binary_cmds_init();
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    extern void cdc_dongle_cmds_init(void);
    cdc_dongle_cmds_init();
    extern void sec_store_init(void);
    extern void cdc_sec_cmds_init(void);
    if (!safe_mode) sec_store_init();   /* NVS read — skip under safe boot */
    cdc_sec_cmds_init();
#if CONFIG_KASE_SEC_OTP_HID
    extern void otp_hid_init(void);
    otp_hid_init();                     /* Wire OTP HID hooks (after sec_store) */
#endif
#if CONFIG_KASE_SEC_OPENPGP
    /* OpenPGP card state. openpgp_card_init() already ran inside
     * kase_tinyusb_init() (CCID driver init: hooks + session + factory PIN
     * baseline) and no longer touches the DO store, so loading/seeding DOs
     * here — after NVS-backed sec_store — is safe. */
    if (!safe_mode) {
      extern void openpgp_do_init(void);
      extern void openpgp_card_load(void);
      extern bool openpgp_card_ensure_defaults(void);
      extern void openpgp_card_set_serial(const uint8_t serial[4]);
      openpgp_do_init();                /* NVS load of persisted DOs */
      openpgp_card_load();              /* NVS load of PIN/retry/key state */
      if (!openpgp_card_ensure_defaults())
        ESP_LOGE(TAG, "openpgp: factory DO seed incomplete");
      uint8_t mac[6] = {0};
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      openpgp_card_set_serial(&mac[2]); /* 4 low MAC bytes = card serial */
    }
#endif
#endif
  }

#if !CONFIG_KASE_NO_KEYMAP_ENGINE
  keymap_init_nvs();
#endif

  if (!safe_mode) {
#if !CONFIG_KASE_NO_KEYMAP_ENGINE
    load_keymaps((uint16_t *)keymaps, KEYMAP_BLOB_BYTES);
    load_layout_names(default_layout_names, LAYERS);
    load_macros(macros_list, MAX_MACROS);
    load_key_stats();
    load_bigram_stats();

    /* Load advanced feature configs */
    extern void tap_dance_load(void);
    extern void combo_load(void);
    extern void leader_load(void);
    tap_dance_load();
    combo_load();
    leader_load();
    key_override_load();
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    bt_devices_load();
#endif
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping NVS config loading");
#endif /* !CONFIG_KASE_NO_KEYMAP_ENGINE */
  }

#if CONFIG_KASE_LINK_WIRE
  /* Lien filaire TRRS. Placé tôt et hors du dispatch de rôles : sa première
   * action est de mettre LINK_5V_EN à BAS, avant que quoi que ce soit d'autre
   * ne touche aux GPIO. */
  link_uart_start();
#endif

#if CONFIG_KASE_HALF_LINK_RX
  /* Moitié gauche à l'écoute de la droite. Placé hors du dispatch de rôles pour
   * la même raison que le probe, et avant kbd_relay_init() qui réclamerait la
   * même radio. */
  half_link_rx_start();
#endif

#if CONFIG_KASE_NRF_PROBE
  /* Diagnostic de banc, HORS du dispatch de rôles : la moitié droite le veut
   * aussi, et elle n'est pas en rôle clavier. Placé avant toute init de rôle
   * car kbd_relay_init() réclame les GPIO de la radio et ouvre le bus SPI —
   * après quoi le test de lignes ne mesurerait plus que ses propres broches. */
  rf_probe_run();
#endif

#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
  /* --- Keyboard-only init: display, matrix, BLE, LED strip --- */
#if CONFIG_KASE_HAS_DISPLAY
  if (!safe_mode) {
    ESP_LOGI(TAG, "display init");
#if !SKIP_STATUS_DISPLAY
    {
      extern const display_backend_t
#if defined(BOARD_DISPLAY_BACKEND_MEMLCD)
          memlcd_display_backend;
      display_set_backend(&memlcd_display_backend);
#elif defined(BOARD_DISPLAY_BACKEND_ROUND)
          round_display_backend;
      display_set_backend(&round_display_backend);
#else
          oled_display_backend;
      display_set_backend(&oled_display_backend);
#endif
    }
    status_display_start();
    xTaskCreatePinnedToCore(status_display_task, "status_disp", 6144, NULL, 2,
                            &status_display_task_handle, 1);
#endif
  } else {
    ESP_LOGW(TAG, "Safe mode: skipping display");
  }
#endif /* CONFIG_KASE_HAS_DISPLAY */

  rtc_matrix_deinit();
  ESP_LOGI(TAG, "Matrix setup init");
  matrix_setup();

  ESP_LOGI(TAG, "Keyboard manager init");
  keyboard_manager_init();

#if CONFIG_KASE_KBD_WIRELESS
  kbd_relay_init();
#endif

  ESP_LOGI(TAG, "Task Matrix init");
  TaskHandle_t xHandleMatrixKeyboard = NULL;
  static uint8_t ucParameterToPass;
  xTaskCreatePinnedToCore(vTaskKeyboard, "Matrix_Keyboard", 6144,
                          &ucParameterToPass, 3, &xHandleMatrixKeyboard, 0);

  if (!safe_mode) {
#if CONFIG_KASE_HAS_BLE
    ESP_LOGI(TAG, "bluetooth init check");
    if (load_bt_state()) {
      ESP_LOGI(TAG, "Starting bluetooth (saved state: ON)");
      init_hid_bluetooth();
      /* Restore last HID output mode (USB/BLE) so BT works on battery boot */
      uint8_t saved_mode = load_io_mode();
      if (saved_mode == 1 && hid_bluetooth_is_initialized()) {
        usb_bl_state = 1;
        ESP_LOGI(TAG, "Restored HID output mode: BLE");
      }
    } else {
      ESP_LOGI(TAG, "Bluetooth disabled (saved state: OFF)");
    }
#endif /* CONFIG_KASE_HAS_BLE */

#if BOARD_HAS_LED_STRIP
    ESP_LOGI(TAG, "LED Strip init");
    led_strip_test();
    led_strip_start_task();
#endif
  }
#elif CONFIG_KASE_DEVICE_ROLE_DONGLE
  /* --- Dongle role: no local matrix/display/BLE. Engine is fed by RF. --- */
  ESP_LOGI(TAG, "Dongle role: init engine + RF RX");
  {
    /* Engine subsystem inits (keyboard_manager_init lives in keyboard_task.c
     * which is not compiled on the dongle — call the pieces directly). */
    /* Plus aucune initialisation de moteur ici : le dongle reçoit du HID déjà
     * fini et n'a plus rien à calculer. Voir
     * docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md */
    extern bool rf_rx_start(void);
    if (!rf_rx_start())
      ESP_LOGE(TAG, "RF RX failed to start (no radios?)");
  }
#elif CONFIG_KASE_DEVICE_ROLE_MOUSE
  /* --- Rôle souris (Conchodytes) : capteur PMW3389, trois clics, molette. ---
   *
   * Ni matrice, ni keymap, ni écran, ni BLE : le CMakeLists ne compile aucun
   * de ces modules pour ce rôle. La tâche ne produit pas encore de rapport
   * HID — le relais vers le slot 2 du dongle est le jalon suivant, avec sa
   * propre spec. */
  ESP_LOGI(TAG, "Role souris : capteur + clics + molette");
  {
    extern esp_err_t mouse_task_start(void);
    esp_err_t err = mouse_task_start();
    if (err != ESP_OK)
      ESP_LOGE(TAG, "mouse_task_start a echoue : %s", esp_err_to_name(err));
  }
#elif CONFIG_KASE_DEVICE_ROLE_NIPHAR_SLAVE
  /* --- Moitié DROITE du Niphargus : un scanner, rien d'autre. ---
   *
   * La spec la décrit comme « un scanner qui remonte sa matrice brute » : elle
   * n'a ni moteur keymap, ni sortie HID — c'est la gauche qui porte les deux.
   * On initialise donc le scan et rien de plus. Le pilote keyboard_button crée
   * sa propre tâche et rappelle notre callback ; aucune tâche à créer ici.
   *
   * Sans cet appel, activer KASE_HAS_LOCAL_MATRIX compilait le module sans
   * jamais l'initialiser : la matrice restait muette et le brochage
   * invérifiable. L'émission vers la gauche est le ressort de B3, pas encore
   * écrite. */
  ESP_LOGI(TAG, "Role esclave Niphargus : scan matrice seul");
#if CONFIG_KASE_HALF_LINK_TX
  /* AVANT matrix_setup() : le callback de scan émettra dès le premier appui,
   * et il lui faut une radio prête. */
  half_link_tx_init();
  /* APRES l'init : la tache reaffirme les maintiens, que le callback de scan ne
   * peut pas produire (il ne se declenche que sur changement). Sans elle, une
   * touche tenue plus de 250 ms est relachee a tort par la gauche. */
  half_link_tx_refresh_start();
#endif
  rtc_matrix_deinit();
  matrix_setup();
#if CONFIG_KASE_DISPLAY_MEMLCD
  /* Écran de la DROITE : la status_display_task du rôle clavier tire le moteur
   * et les stats, absents ici. Une tâche minimale suffit : init du backend,
   * update() toutes les 100 ms (redessin à la demande + entretien VCOM), et la
   * veille lui est signalée par veille.c (image gelée). */
  {
    extern const display_backend_t memlcd_display_backend;
    display_set_backend(&memlcd_display_backend);
    xTaskCreatePinnedToCore(memlcd_slave_display_task, "memlcd_disp", 4096, NULL, 2, NULL, 1);
  }
#endif
#endif /* device role */

#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
  /* CPU-time logger is a dev diagnostic; runtime stats aren't enabled on the
   * dongle build (it would just fail every 5s), so keyboard-only. */
  xTaskCreatePinnedToCore(cpu_time_logger_task, "cpu_time", 4096, NULL, 2, NULL,
                          1);
#endif

  /* Boot succeeded — reset crash counter and validate OTA */
  boot_crash_count = 0;
  esp_ota_mark_app_valid_cancel_rollback();
  ESP_LOGI(TAG, "Boot OK%s", safe_mode ? " (SAFE MODE)" : "");

  for (;;) {
    // keep main light; display handled in its own task
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
