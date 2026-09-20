/* Dynamic Frequency Scaling (DFS) — ESP-IDF power management.
 *
 * The Niphargus's cost is not sleep (244 µA) but IDLE-AWAKE: cores
 * idle at 160 MHz = 27.6 mA, at 40 MHz = 13.2 mA (ESP32-S3 datasheet v2.2,
 * table 5-9, p. 67; 42 / 19 mA in the typ2 column with PSRAM — the module is an
 * N16R8). esp_pm holds the processor at MAX frequency as long as a task
 * is running and drops it to MIN as soon as both cores are idle: typing doesn't
 * change, waiting costs half as much.
 *
 * What ESP-IDF 5.5 handles alone: the UART console switches back to XTAL
 * (esp_pm_impl_init), the SPI master (radio, screen), the gptimer (scan) and
 * the ADC (gauge) take an APB_FREQ_MAX lock for the duration of their transactions.
 * What it doesn't handle: the TRRS link's UART1 (XTAL source set in
 * link_uart.c) and the USB OTG, which needs the PLL — at 40 MHz on XTAL it
 * is cut off. So we hold an APB_FREQ_MAX lock as long as a host is mounted.
 * Cold plug-in still works (bench 2026-09-16, left idle at
 * 40 MHz with automatic sleeps: cafe:4003 enumerates, route=USB, lock
 * held — a plug-in failure turned out to be a charge-only cable).
 *
 * Automatic light sleep (tickless) under CONFIG_FREERTOS_USE_TICKLESS_IDLE:
 * see pm_dfs_init. Manual sleep (veille.c, esp_light_sleep_start) is
 * independent of DFS. */
#include "pm_dfs.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_log.h"
#include "tinyusb.h"
#if CONFIG_KASE_VEILLE && CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "veille_task.h"   /* veto USB : un hôte attend un clavier */
#endif

static const char *TAG = "pm_dfs";
static esp_pm_lock_handle_t s_usb_lock;
static bool s_usb_tenu;

/* APB lock held as long as a host is mounted. Driven by TinyUSB EVENTS
 * (mount / unmount), not by polling: a 200 ms timer used to pull
 * the processor out of idle five times a second just to read a boolean. */
static void usb_hote(bool monte)
{
    if (!s_usb_lock) return;
    if (monte && !s_usb_tenu)       { esp_pm_lock_acquire(s_usb_lock); s_usb_tenu = true;  ESP_LOGI(TAG, "hote USB monte : APB tenu a 80 MHz"); }
    else if (!monte && s_usb_tenu)  { esp_pm_lock_release(s_usb_lock); s_usb_tenu = false; ESP_LOGI(TAG, "hote USB parti : DFS libre"); }
}
/* TinyUSB event (tinyusb_config_t.event_cb, set by usb_hid.c):
 * esp_tinyusb has tud_mount_cb/tud_umount_cb, we go through its relay. */
void pm_dfs_usb_event(bool monte)
{
    usb_hote(monte);
#if CONFIG_KASE_VEILLE && CONFIG_KASE_DEVICE_ROLE_KEYBOARD
    /* The LEFT does not sleep while plugged in (HID keyboard, a host is waiting). The
     * right has no USB veto: its port is just a CDC nobody opens, it
     * sleeps while plugged in (deep, veille.c). The sleep task catches up every 1 s
     * via tud_ready(): unmounting is not always signaled on the S3. */
    veille_veto(VEILLE_VETO_USB, monte);
#endif
}

void pm_dfs_init(void)
{
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,          /* 40 MHz: PLL cut off at rest */
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        /* Sleep BETWEEN keystrokes: automatic light sleep as soon as all
         * tasks have been blocked ≥ FREERTOS_IDLE_TIME_BEFORE_SLEEP ticks and
         * no lock is held (a mounted USB host holds one). GPIO wakeup
         * on the lines (armed by the scan driver in power-saving
         * mode), esp_timer, tick. Sleep B7 (veille.c) stays on top of this
         * for long absences: radio off, then deep sleep. */
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,               /* veille.c handles it, manually */
#endif
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_pm_configure: %s", esp_err_to_name(e)); return; }
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb_hote", &s_usb_lock));
    usb_hote(tud_mounted());   /* in case the host enumerated before us */
    ESP_LOGW(TAG, "DFS actif : %d MHz en travail, %d MHz oisif (PLL coupee) ; light sleep auto : %s ; hote USB => APB 80 MHz",
             cfg.max_freq_mhz, cfg.min_freq_mhz, cfg.light_sleep_enable ? "OUI" : "non");
}
#else
void pm_dfs_init(void) {}
void pm_dfs_usb_event(bool monte) { (void)monte; }
#endif
