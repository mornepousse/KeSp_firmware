/*
 * KeSp — keyboard firmware framework
 */
#include "cdc_acm_com.h"
#include "cpu_time.h"
#include "veille_task.h"   /* pure outside CONFIG_KASE_VEILLE (veto/hook: calls under #if) */
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
#include "memlcd_panel.h"   /* memlcd_cs_idle: display CS low from boot (shared nRF24 bus) */
#endif
#if CONFIG_KASE_VEILLE
#include "veille.h"
#endif
#include "pm_dfs.h"   /* vide sans CONFIG_PM_ENABLE */
#include "cadence.h"  /* HB_PERIODE_MS, MEMLCD_DROITE_PERIODE_MS */
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
#include "display_backend.h"   /* any role with a display (keyboard, right half) */
#endif
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "hid_bluetooth_manager.h"   /* real API, or no-op stubs when HAS_BLE off */
#include "keyboard_task.h"
#include "led_strip_anim.h"
#include "status_display.h"
#endif

/* matrix_scan.h follows the matrix, not the role: the Niphargus right half
 * also scans (KASE_HAS_LOCAL_MATRIX) without being a full keyboard. */
#if CONFIG_KASE_HAS_LOCAL_MATRIX
#include "matrix_scan.h"
#endif

#if CONFIG_KASE_KBD_WIRELESS
#include "kbd_relay_tx.h"
#include "usb_presence.h"   /* kbd_active_route — displayed via the heartbeat */
#endif

/* Bench diagnostic, role-independent: the Niphargus left half has neither
 * KBD_WIRELESS nor HAS_RF_RX, so the include must live outside these blocks. */
#if CONFIG_KASE_NRF_PROBE
#include "rf_probe.h"
#endif

#if CONFIG_KASE_HALF_LINK_TX
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

#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD && !CONFIG_KASE_VEILLE
/* KaSe V1/V2: bench heartbeat. On the Niphargus halves
 * (KASE_VEILLE) the sleep task carries it (power/veille_task.c). */
static void cpu_time_logger_task(void *arg) {
  (void)arg;
  char buf[512];
  for (;;) {
    if (cpu_time_measure_period(1000, buf, sizeof(buf)) == 0) {
      ESP_LOGI(TAG, "CPU usage:\n%s", buf);
    } else {
      /* FreeRTOS runtime stats are not compiled in
       * (CONFIG_FREERTOS_USE_TRACE_FACILITY absent): this message fired every
       * 5 s learning nothing. We turn it into a heartbeat, the only
       * sign of life when the board runs on battery — USB is then
       * unplugged and says nothing more. It also shows the routing, which
       * lets us verify the USB -> RF switch actually happened. */
      uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
      uint32_t dodo_n = 0, dodo_ms = 0;
#if CONFIG_KASE_VEILLE
      veille_bilan(&dodo_n, &dodo_ms);   /* "slept X s out of Y": reads a whole night at a glance */
#endif
      uint32_t inactif_s = (uint32_t)((esp_timer_get_time() / 1000) - get_last_activity_time_ms()) / 1000;
#if CONFIG_PM_PROFILING
      esp_pm_dump_locks(stdout);   /* bench: time spent per mode (light sleep, APB min/max) */
#endif
#if CONFIG_KASE_KBD_WIRELESS
      ESP_LOGW(TAG, "HB up=%us idle=%us slept=%us/%u route=%s relay=%s", (unsigned)up_s,
               (unsigned)inactif_s, (unsigned)(dodo_ms / 1000), (unsigned)dodo_n,
               (kbd_active_route() == KBD_OUT_RF) ? "RF" : "USB",
               kbd_relay_active() ? "active" : "inactive");
#else
      ESP_LOGW(TAG, "HB up=%us idle=%us slept=%us/%u", (unsigned)up_s, (unsigned)inactif_s,
               (unsigned)(dodo_ms / 1000), (unsigned)dodo_n);
#endif
    }
    /* 10 s: this heartbeat is a bench witness (inactive, slept, route), not
     * a service; at 2 s it cost a serial line and an idle print
     * every two seconds. Enough to read back a whole night. */
    vTaskDelay(pdMS_TO_TICKS(HB_PERIODE_MS));
  }
}
#endif

#if CONFIG_KASE_HAS_DISPLAY
#if CONFIG_KASE_DEVICE_ROLE_SPLIT_SCANNER && CONFIG_KASE_DISPLAY_MEMLCD
/* RIGHT half display: minimal task, see the call in app_main (slave
 * role). The keyboard task below drives the engine and stats, which are absent here. */
static void memlcd_slave_display_task(void *arg) {
  (void)arg;
  const display_backend_t *be = display_get_backend();
  if (!be || !be->init()) { ESP_LOGW(TAG, "right screen: init KO"); vTaskDelete(NULL); return; }
  for (;;) { be->update(); vTaskDelay(pdMS_TO_TICKS(MEMLCD_DROITE_PERIODE_MS)); }
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

    /* The window must outlast one period of this task, or a keypress seen
     * late would never wake the screen (STATUS_DISP_PERIODE_MS is 1 s on the
     * memory-LCD halves since 2026-09-25). */
    if (display_sleep && last != 0 && (now - last) <= STATUS_DISP_PERIODE_MS + 500u) {
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
    key_stats_check_save();   /* no-op on the halves: CONFIG_KASE_KEY_STATS=n */
    {
      static uint32_t last_wpm_tick = 0;
      uint32_t now_wpm = esp_timer_get_time() / 1000;
      if (now_wpm - last_wpm_tick > 1000) {
        wpm_tick();
        last_wpm_tick = now_wpm;
      }
    }


    vTaskDelay(pdMS_TO_TICKS(STATUS_DISP_PERIODE_MS));   /* power/cadence.h */
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
  /* Where does this boot come from? A deep sleep wake-up is a REBOOT:
   * without this line, it is indistinguishable from a power-on or a
   * crash, and the log gives no way to tell whether EXT1 worked.
   * Tested on the bench on 2026-09-08: rst:0x5 (DSLEEP), Boot OK at 702 ms. */
  {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1)
      ESP_LOGW(TAG, "EXT1 wake: a key pulled the board out of deep sleep");
    else if (cause != ESP_SLEEP_WAKEUP_UNDEFINED)
      ESP_LOGW(TAG, "sleep wake, cause=%d", (int)cause);
  }
  /* BEFORE any matrix configuration: the columns may still be
   * frozen by the RTC hold set before deep sleep. */
  veille_liberer_gpio();
#endif
#if CONFIG_KASE_DISPLAY_MEMLCD
  /* Display on the SPI bus SHARED with the radio: its CS (active high) is held LOW
   * right now, BEFORE any radio init, so it never listens on the bus. */
  memlcd_cs_idle();
#endif
#if CONFIG_KASE_BATT_SENSE
  /* Gauge: first sample at boot, then every 10 s and on every wake-up. */
  batt_sense_init();
#endif
  /* Dynamic frequency: 160 MHz while working, 40 MHz idle (power/pm_dfs.c).
   * No-op without CONFIG_PM_ENABLE (Niphargus halves only). */
  pm_dfs_init();

  if (boot_crash_count > BOOT_CRASH_LIMIT) {
    ESP_LOGW(TAG, "Crash loop detected (%lu boots) — SAFE MODE",
             (unsigned long)boot_crash_count);
    safe_mode = true;
    boot_crash_count = 0;
    /* Don't erase NVS — user data is precious.
       Safe mode just skips display/BLE/NVS loading. */
  }

  /* ── NVS, for ALL roles ────────────────────────────────────────────
   *
   * It used to be initialized only by keymap_init_nvs(), in input/keymap.c,
   * behind the keymap engine's guard. Neither the dongle nor the mouse compile
   * that file — nor does hid_bluetooth_manager.c, the other caller.
   *
   * Consequence, found on the bench on 2026-08-26: the dongle replied
   * ESP_ERR_NVS_NOT_INITIALIZED (0x1101) to every write. It acknowledged
   * pairing requests without ever saving anything, with no way
   * to say so — it runs with no console in production. The mouse showed
   * exactly the same symptom, for exactly the same reason.
   *
   * NVS is infrastructure: it has no business being behind a role
   * guard. keymap_init_nvs() is still called further down for keyboard roles;
   * a second nvs_flash_init() has no effect. */
  {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_LOGW(TAG, "NVS to erase and reinitialize (%s)", esp_err_to_name(nvs));
      ESP_ERROR_CHECK(nvs_flash_erase());
      nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK)
      ESP_LOGE(TAG, "NVS unavailable: %s — pairing and config will not be "
                    "persisted", esp_err_to_name(nvs));
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
  /* Wired TRRS link. Placed early and outside the role dispatch: its first
   * action is to set LINK_5V_EN LOW, before anything else
   * touches the GPIOs. */
  link_uart_start();
#endif


#if CONFIG_KASE_NRF_PROBE
  /* Bench diagnostic, OUTSIDE the role dispatch: the right half wants it
   * too, and it isn't in a keyboard role. Placed before any role init
   * because kbd_relay_init() claims the radio's GPIOs and opens the SPI bus —
   * after which the line test would only measure its own pins. */
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
    /* No more engine initialization here: the dongle receives HID that is
     * already finished and has nothing left to compute. See
     * docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md */
    extern bool rf_rx_start(void);
    if (!rf_rx_start())
      ESP_LOGE(TAG, "RF RX failed to start (no radios?)");
  }
#elif CONFIG_KASE_DEVICE_ROLE_MOUSE
  /* --- Mouse role (Conchodytes): PMW3389 sensor, three clicks, wheel. ---
   *
   * No matrix, no keymap, no display, no BLE: the CMakeLists compiles none
   * of these modules for this role. The task does not yet produce an HID
   * report — relaying to the dongle's slot 2 is the next milestone, with its
   * own spec. */
  ESP_LOGI(TAG, "Mouse role: sensor + clicks + wheel");
  {
    extern esp_err_t mouse_task_start(void);
    esp_err_t err = mouse_task_start();
    if (err != ESP_OK)
      ESP_LOGE(TAG, "mouse_task_start failed: %s", esp_err_to_name(err));
  }
#elif CONFIG_KASE_DEVICE_ROLE_SPLIT_SCANNER
  /* --- Niphargus RIGHT half: a scanner, nothing else. ---
   *
   * The spec describes it as "a scanner that reports its raw matrix": it
   * has neither a keymap engine nor an HID output — the left half carries both.
   * So we only initialize the scan and nothing more. The keyboard_button driver
   * creates its own task and calls our callback back; no task to create here.
   *
   * Without this call, enabling KASE_HAS_LOCAL_MATRIX compiled the module without
   * ever initializing it: the matrix stayed silent and the pinout was
   * unverifiable. Transmitting to the left half is B3's job, not yet
   * written. */
  ESP_LOGI(TAG, "Niphargus slave role: scan matrix only");
#if CONFIG_KASE_HALF_LINK_TX
  /* BEFORE matrix_setup(): the scan callback will transmit from the first press,
   * and it needs a radio that is ready. */
  half_link_tx_init();
  /* AFTER init: the task reaffirms held keys, which the scan callback
   * cannot produce (it only fires on change). Without it, a
   * key held for more than 250 ms is wrongly released by the left half. */
  half_link_tx_refresh_start();
#endif
  rtc_matrix_deinit();
  matrix_setup();
#if CONFIG_KASE_DISPLAY_MEMLCD
  /* RIGHT half display: the keyboard role's status_display_task drives the engine
   * and stats, which are absent here. A minimal task is enough: backend init,
   * update() every 100 ms (redraw on demand + VCOM upkeep), and
   * sleep is signaled to it by veille.c (frozen image). */
  {
    extern const display_backend_t memlcd_display_backend;
    display_set_backend(&memlcd_display_backend);
    xTaskCreatePinnedToCore(memlcd_slave_display_task, "memlcd_disp", 4096, NULL, 2, NULL, 1);
  }
#endif
#endif /* device role */

#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD && !CONFIG_KASE_VEILLE
  /* CPU-time logger is a dev diagnostic; runtime stats aren't enabled on the
   * dongle build (it would just fail every 5s), so keyboard-only. */
  xTaskCreatePinnedToCore(cpu_time_logger_task, "cpu_time", 4096, NULL, 2, NULL,
                          1);
#endif
#if CONFIG_KASE_VEILLE
  /* ONE sleep task for both halves: inactivity, vetoes (USB, link,
   * sync, test), hooks (radio, display, gauge), heartbeat. Started
   * after the modules; a hook registered later (display, in its own task)
   * counts from the very next sleep. */
  veille_task_start();
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
