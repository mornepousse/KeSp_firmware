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
#include "espnow_link.h"      /* espnow_reload_peers / espnow_link_restart_espnow */
#include "esp_wifi.h"
#include "esp_now.h"          /* esp_now_deinit */
#endif

#if CONFIG_KASE_HAS_TRACKPAD
#include "trackpad.h"
#endif

static const char *TAG = "half_sleep";

void half_sleep_enter(void)
{
    ESP_LOGI(TAG, "SLEEP: quiesce");
    half_scan_stop_for_sleep();      /* stop heartbeat + delete keyboard_button */

    /* ── Step 1: tear down ESP-NOW + WiFi BEFORE acquiring any SPI lock.
     *
     * ROOT CAUSE of the esp_wifi_stop() hang on the e-ink half (confirmed):
     *
     * esp_wifi_stop() is synchronous: internally it sends a stop command to the
     * WiFi ppTask (prio 23 in libpp.a) then posts a WIFI_EVENT_STA_STOP event
     * to the default event loop. The event loop task (ESP_TASKD_EVENT_PRIO =
     * configMAX_PRIORITIES-5 = 20) must drain that event and run the
     * wifi_default_action_sta_stop() callback before esp_wifi_stop() returns.
     *
     * The hang occurs because the event task calls into lwIP / netif machinery
     * which calls malloc() (via wifi_malloc → heap_caps_malloc_prefer). The IDF
     * heap allocator uses a per-region spinlock (multi_heap_lock). On the e-ink
     * half, the eink_lvgl_task (prio 3) is in the middle of lv_timer_handler()
     * when we call esp_wifi_stop(). lv_timer_handler → lv_obj_invalidate →
     * lv_mem_alloc → heap_caps_malloc holds the internal-RAM heap spinlock.
     * The event task trying to free/alloc WiFi buffers spins forever on that
     * same lock → deadlock.
     *
     * This does NOT happen on the half without an e-ink: no lv_timer_handler
     * runs, so the heap spinlock is never contended.
     *
     * Note: half_spi_lock is NOT yet held at this point — the e-ink task can
     * still run freely. That is intentional: we want eink_lvgl_task to complete
     * any lv_timer_handler pass it is in (it will call eink_push → try to take
     * half_spi_lock → block there). Once eink_lvgl_task is blocked on
     * half_spi_lock (not on the heap), the heap is free, and esp_wifi_stop()
     * can complete its event-loop drain without contention.
     *
     * Fix: (1) esp_now_deinit() first — cleans up the ESP-NOW layer cleanly
     * before WiFi stops; (2) esp_wifi_stop() while the heap is uncontested
     * (eink_lvgl_task hasn't been blocked yet); (3) THEN acquire half_spi_lock
     * for the SPI-critical window.  This is "option C" from the analysis:
     * reorder WiFi stop to BEFORE the SPI lock — cheapest, safest fix. */
#if CONFIG_KASE_HAS_ESPNOW
    esp_now_deinit();                /* unregister recv cb + free ESP-NOW resources */
    esp_wifi_stop();                 /* WiFi off — heap uncontested, event task runs freely */
    ESP_LOGI(TAG, "SLEEP: WiFi stopped");
#endif

    /* ── Step 2: hold the shared SPI2 bus mutex across the NRF + sleep window.
     *
     * Once we take half_spi_lock, the eink_lvgl_task blocks on it (inside
     * eink_push's half_spi_lock() call) — it is now OUTSIDE any IDF SPI
     * transaction and OUTSIDE any heap alloc. Safe to power-cycle the NRF and
     * enter light-sleep.
     *
     * See the original comment in commit history for the full SPI-bus-lock
     * inversion analysis (vTaskSuspend mid-transaction story). */
    half_spi_lock();

#if CONFIG_KASE_HAS_EINK
    eink_lvgl_suspend();             /* stop 5 ms LVGL tick (no vTaskSuspend) */
#endif
#if CONFIG_KASE_HAS_TRACKPAD
    trackpad_suspend();              /* trackpad blocked on the mutex → safe to suspend */
#endif
    half_scan_nrf_power(false);      /* NRF power-down — SPI under half_spi_lock */

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
    espnow_link_restart_espnow();    /* esp_now_init + reload_peers + register_recv_cb */
#endif
#if CONFIG_KASE_HAS_EINK
    eink_lvgl_resume();
#endif
#if CONFIG_KASE_HAS_TRACKPAD
    trackpad_resume();
#endif
}
