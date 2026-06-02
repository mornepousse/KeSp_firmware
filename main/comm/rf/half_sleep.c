#include "half_sleep.h"
#include "half_scan_task.h"   /* half_scan_stop_for_sleep / restart_after_wake /
                               * half_scan_nrf_power / arm/disarm_key_wake */
#include "esp_sleep.h"        /* esp_light_sleep_start */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_KASE_HAS_EINK
#include "eink_lvgl.h"        /* eink_lvgl_suspend / resume */
#endif

#if CONFIG_KASE_HAS_ESPNOW
#include "espnow_link.h"      /* espnow_reload_peers */
#include "esp_wifi.h"
#endif

#if CONFIG_KASE_HAS_TRACKPAD
#include "trackpad.h"
#endif

static const char *TAG = "half_sleep";

void half_sleep_enter(void)
{
    ESP_LOGI(TAG, "SLEEP: quiesce");
    half_scan_stop_for_sleep();      /* stop heartbeat + delete keyboard_button */
#if CONFIG_KASE_HAS_EINK
    eink_lvgl_suspend();             /* stop 5 ms LVGL tick + suspend task */
#endif
#if CONFIG_KASE_HAS_TRACKPAD
    trackpad_suspend();
#endif
#if CONFIG_KASE_HAS_ESPNOW
    esp_wifi_stop();                 /* WiFi off (main saving) */
#endif

    /* NRF power-down → arm key-wake GPIOs → light-sleep.
     * esp_light_sleep_start() blocks until a key GPIO fires (or any other enabled
     * wake source). BENCH-TUNE: GPIO polarity (rows HIGH / cols pulldown / HIGH level)
     * is first-cut — verify on hardware; invert if keypress does not trigger wake. */
    half_scan_nrf_power(false);
    half_scan_arm_key_wake();
    ESP_LOGI(TAG, "SLEEP: entering light-sleep");
    esp_light_sleep_start();         /* returns on wake */
    half_scan_disarm_key_wake();

    ESP_LOGI(TAG, "WAKE: restore");
    half_scan_nrf_power(true);       /* NRF power-up FIRST — Tpd2stby wait inside */
    half_scan_restart_after_wake();  /* recreate keyboard_button + heartbeat; detects held key */
#if CONFIG_KASE_HAS_ESPNOW
    esp_wifi_start();
    espnow_reload_peers();
#endif
#if CONFIG_KASE_HAS_EINK
    eink_lvgl_resume();
#endif
#if CONFIG_KASE_HAS_TRACKPAD
    trackpad_resume();
#endif
}
