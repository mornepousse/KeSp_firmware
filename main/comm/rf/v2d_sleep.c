/* v2d_sleep.c — V2D wireless RF light-sleep + matrix-keypress wake.
 * Mirrors half_sleep.c, adapted for the V2D (OLED I2C + kbd_relay NRF + full
 * matrix). Spec: docs/superpowers/specs/2026-06-28-v2d-rf-light-sleep-design.md.
 * Compiled only when CONFIG_KASE_KBD_WIRELESS (CMakeLists guard). */
#include "v2d_sleep.h"
#include "matrix_scan.h"        /* rtc_matrix_deinit / matrix_setup / arm/disarm wake */
#include "kbd_relay_tx.h"       /* kbd_relay_sleep_prepare / wake_restore */
#include "status_display.h"     /* status_display_sleep / wake */
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"      /* lvgl_port_lock / unlock — quiesce LVGL vs WiFi-stop */

#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_link.h"        /* espnow_link_restart_espnow */
#include "esp_wifi.h"
#include "esp_now.h"            /* esp_now_deinit */
#endif

extern volatile uint32_t last_activity_time_ms;   /* matrix_scan.c */

static const char *TAG = "v2d_sleep";

void v2d_sleep_enter(void)
{
    ESP_LOGI(TAG, "SLEEP: quiesce (RF idle)");

    /* OLED panel off (takes/releases lvgl_port_lock internally — do it BEFORE we
     * grab and hold the lock ourselves). */
    status_display_sleep();

    /* Hold the LVGL lock across the WiFi teardown + sleep: this blocks the LVGL
     * port task OUTSIDE lv_timer_handler, so it can't hold the heap spinlock while
     * esp_wifi_stop() drains its event (the half's documented eink deadlock). */
    bool lvgl_held = lvgl_port_lock(1000);
    if (!lvgl_held) ESP_LOGW(TAG, "SLEEP: lvgl_port_lock timeout — proceeding");

#if CONFIG_KASE_HAS_ESPNOW
    esp_now_deinit();
    esp_wifi_stop();
    ESP_LOGI(TAG, "SLEEP: WiFi stopped");
#endif

    rtc_matrix_deinit();          /* stop the scan driver (releases the matrix GPIOs) */
    kbd_relay_sleep_prepare();    /* refresh timer off, NRF power-down (holds TX mutex) */

    matrix_arm_key_wake();
    ESP_LOGI(TAG, "SLEEP: entering light-sleep");
    esp_light_sleep_start();      /* returns on a matrix keypress */
    matrix_disarm_key_wake();
    ESP_LOGI(TAG, "WAKE: restore");

    kbd_relay_wake_restore();     /* NRF power-up, refresh timer on */
    matrix_setup();               /* recreate the scan driver (re-reads held keys) */

#if CONFIG_KASE_HAS_ESPNOW
    esp_wifi_start();
    espnow_link_restart_espnow();
#endif

    if (lvgl_held) lvgl_port_unlock();
    status_display_wake();        /* OLED back on */

    /* Stamp activity now so we don't immediately re-evaluate the idle window
     * (the waking keypress also bumps it via the scan callback). */
    last_activity_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
}
