#include "half_sleep.h"
#include "half_scan_task.h"   /* half_scan_stop_for_sleep / restart_after_wake */
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

    /* TODO Task 6: NRF power-down + matrix GPIO wake config + esp_light_sleep_start().
     * Skeleton placeholder: a fixed 2 s delay. Task 6's esp_light_sleep_start() will
     * block HERE until a key GPIO wakes the CPU, so the wifi_stop/start cycle runs
     * once per wake event — not every 2.5 s. The cycling below is INTENTIONAL
     * skeleton behaviour, not a bug. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "WAKE: restore");
    /* TODO Task 6: NRF power-up FIRST */
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
