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

    /* NRF power-down MUST happen BEFORE eink_lvgl_suspend().
     *
     * ROOT CAUSE of the hang (confirmed from IDF spi_master.c source):
     *
     * spi_device_polling_transmit() = polling_start() + polling_end().
     * polling_start() calls spi_bus_lock_acquire_start(dev->dev_lock, portMAX_DELAY).
     * Inside acquire_start(): if another device currently holds the IDF bus lock
     * (i.e. lock->acquiring_dev != NULL and its LOCK bit is set), acquire_core()
     * returns false and the caller blocks on xSemaphoreTake(dev->semphr, portMAX_DELAY).
     * That semaphore is ONLY given by the current owner when it calls polling_end() →
     * spi_bus_lock_acquire_end(), which runs acquire_end_core() → resume_dev() on the
     * next waiting device.
     *
     * When eink_lvgl_task is vTaskSuspend()'d by eink_lvgl_suspend(), the task is
     * frozen at whatever FreeRTOS scheduling point the kernel chose.  If it was
     * suspended AFTER entering polling_start (i.e. after spi_bus_lock_acquire_start
     * set the e-ink device's LOCK bit and made it lock->acquiring_dev) but BEFORE
     * polling_end() cleared the LOCK bit and called spi_bus_lock_acquire_end(), the
     * IDF bus lock is permanently stranded:
     *   - lock->acquiring_dev == s_eink_dev  (LOCK bit set, never cleared)
     *   - The NRF polling_start() calls acquire_core(), sees LOCK_MASK != 0,
     *     returns false, then blocks on xSemaphoreTake(nrf->semphr, portMAX_DELAY)
     *   - That semaphore will never be given because the only task that can call
     *     polling_end() (eink_lvgl_task) is suspended.
     *   → PERMANENT DEADLOCK, indistinguishable from a bare hang.
     *
     * The fix: power down the NRF (two SPI transactions) BEFORE suspending the
     * e-ink task.  At that point:
     *   - The heartbeat timer is already stopped (half_scan_stop_for_sleep above),
     *     so no new NRF SPI traffic will be attempted.
     *   - The e-ink task may or may not hold the lock, but it doesn't matter:
     *     we are not competing for the bus here.
     *   - The NRF power-down writes complete while the bus is fully idle (no e-ink
     *     refresh is triggered between heartbeat stop and this point, and the LVGL
     *     task has not been suspended yet so if it was mid-transaction it can still
     *     finish and release the lock normally before we try to acquire it).
     *
     * The half_spi_lock() (FreeRTOS mutex, separate from the IDF bus lock) is still
     * needed here: the keyboard_button task was deleted above, and the heartbeat timer
     * is stopped, so there are no concurrent NRF callers left from the scan side —
     * BUT eink_push() (called from flush_cb inside lv_timer_handler) also takes
     * half_spi_lock while doing its SPI writes.  We wrap the NRF power-down in
     * half_spi_lock so it cannot collide with an in-progress eink_push.
     *
     * After half_spi_unlock() returns, the eink task is guaranteed to be OUTSIDE any
     * polling transaction (it had to release half_spi_lock to reach this point), so
     * it cannot hold the IDF bus lock.  It is then safe to vTaskSuspend it. */
    half_spi_lock();
    half_scan_nrf_power(false);      /* power down NRF FIRST — see comment above */
    half_spi_unlock();

#if CONFIG_KASE_HAS_EINK
    eink_lvgl_suspend();             /* stop 5 ms LVGL tick + suspend task (now safe) */
#endif
#if CONFIG_KASE_HAS_TRACKPAD
    trackpad_suspend();
#endif
#if CONFIG_KASE_HAS_ESPNOW
    esp_wifi_stop();                 /* WiFi off (main saving) */
#endif
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
