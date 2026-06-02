#include "half_sleep.h"
#include "half_scan_task.h"   /* half_scan_stop_for_sleep / restart_after_wake /
                               * half_scan_nrf_power / arm/disarm_key_wake */
#include "half_spi.h"         /* half_spi_lock / half_spi_unlock */
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

    /* Hold the shared SPI2 bus mutex (half_spi_lock) across the ENTIRE sleep
     * window — NRF power-down, the light-sleep, and NRF power-up. This is what
     * makes light-sleep safe on e-ink halves (NRF + e-ink share SPI2).
     *
     * ROOT CAUSE (confirmed from IDF spi_master.c): spi_device_polling_transmit
     * acquires the IDF SPI bus lock for the transaction. If a task is frozen
     * mid-transaction (holding that lock), the next polling_transmit on the other
     * device blocks forever (xSemaphoreTake portMAX_DELAY). Two ways the e-ink
     * froze mid-transaction: (a) vTaskSuspend(eink) at an arbitrary point —
     * now removed (eink_lvgl_suspend only stops the tick); (b) esp_light_sleep
     * CPU-pausing the e-ink task while it is mid eink_push, then priority
     * inversion on wake (high-prio scan task's NRF power-up waits for the
     * low-prio e-ink task to release the bus, but it can't run).
     *
     * Holding half_spi_lock for the whole window prevents BOTH: eink_push() takes
     * half_spi_lock around its SPI, so while WE hold it the e-ink task can only be
     * blocked on the mutex (OUTSIDE any IDF transaction) — never holding the IDF
     * bus lock when we power the NRF or when the chip sleeps/wakes. */
    half_spi_lock();

#if CONFIG_KASE_HAS_EINK
    eink_lvgl_suspend();             /* stop 5 ms LVGL tick (no vTaskSuspend) */
#endif
#if CONFIG_KASE_HAS_TRACKPAD
    trackpad_suspend();              /* trackpad blocked on the mutex → safe to suspend */
#endif
    half_scan_nrf_power(false);      /* NRF power-down — SPI under half_spi_lock */
#if CONFIG_KASE_HAS_ESPNOW
    esp_wifi_stop();                 /* WiFi off (main saving) */
#endif
    half_scan_arm_key_wake();
    ESP_LOGI(TAG, "SLEEP: entering light-sleep");
    esp_light_sleep_start();         /* returns on wake (still holding half_spi_lock) */
    half_scan_disarm_key_wake();

    ESP_LOGI(TAG, "WAKE: restore");
    half_scan_nrf_power(true);       /* NRF power-up — SPI under half_spi_lock */
    half_scan_restart_after_wake();  /* recreate keyboard_button + heartbeat; detects held key */
    half_spi_unlock();               /* release the bus: e-ink/heartbeat resume SPI */
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
