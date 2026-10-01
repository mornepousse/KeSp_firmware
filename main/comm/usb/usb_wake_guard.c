/* HW glue for usb_wake_guard.h — see there for the root cause and why. */
#include "usb_wake_guard.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/* Only matters where automatic light sleep can actually race with
 * enumeration — gated the same way pm_dfs.c's own USB lock is. The dongle
 * (CONFIG_PM_ENABLE=n, mains-powered, no battery budget to protect) compiles
 * this out entirely. */
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE

static const char *TAG = "usb_wake";

/* D+ on the ESP32-S3's built-in full-speed USB-OTG PHY (ESP-IDF "Establish
 * Serial Connection with ESP32-S3": GPIO20 = D+, GPIO19 = D-; TRM "USB 2.0
 * OTG Full-Speed Interface", p. 55 — both confirmed via lemia, esp32s3 tag).
 * Fixed silicon pins, not board wiring: same on both halves, no board.h
 * entry needed.
 *
 * Deliberately NOT touched by gpio_config(): that would reassign the pad's
 * IO_MUX function away from the OTG PHY and disconnect a live USB session —
 * the ESP-IDF "Configure USB PHY Pins to GPIO" recipe is for when the OTG
 * peripheral is NOT in use, not for watching it from the side. Only the
 * interrupt/wakeup machinery is attached below, which reads the pad's input
 * level in parallel with whatever peripheral drives it (the same parallel
 * read this firmware already relies on for other multiplexed pins — UART0
 * TX/RX doubling as matrix columns, see matrix_setup()). NOT bench-proven on
 * real hardware yet (see docs/HARDWARE_SMOKE_TEST.md): if the D+ pad turns
 * out to bypass the GPIO matrix entirely for input too, this silently does
 * nothing (soft failure, not a regression) — the log line below is the way
 * to tell on the bench. */
#define USB_DP_GPIO GPIO_NUM_20

static esp_pm_lock_handle_t s_lock;
static usb_wake_guard_t     s_guard;
static volatile bool        s_log_pending;   /* ISR -> tick(), see usb_wake_guard_tick */

/* IRAM-safe by convention (matches nrf_irq_isr / chest irq_isr): registered
 * without ESP_INTR_FLAG_IRAM, so ESP-IDF simply won't invoke it while flash
 * cache is disabled — it does not need to avoid flash-resident calls.
 * NEGEDGE, not level: D+ stays low for the whole duration of a bus reset
 * (SE0, ~10-20 ms) or a packet's K-state bits; a level interrupt would
 * re-fire continuously for that whole window. An edge fires once per
 * transition — bursts are bounded by packet length (tens of us) with gaps
 * far longer in between, never a sustained storm (see usb_wake_guard.h). */
static void IRAM_ATTR dp_isr(void *arg)
{
    (void)arg;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (usb_wake_guard_activity(&s_guard, now_ms)) {
        s_log_pending = true;
        if (s_lock) esp_pm_lock_acquire(s_lock);   /* esp_pm.h: may be called from an ISR */
    }
}

void usb_wake_guard_init(void)
{
    esp_err_t e = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_wake", &s_lock);
    if (e != ESP_OK) { ESP_LOGE(TAG, "lock create: %s", esp_err_to_name(e)); return; }

    gpio_set_intr_type(USB_DP_GPIO, GPIO_INTR_NEGEDGE);
    esp_err_t ei = gpio_install_isr_service(0);
    if (ei != ESP_OK && ei != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(ei)); return; }
    gpio_isr_handler_add(USB_DP_GPIO, dp_isr, NULL);
    gpio_intr_enable(USB_DP_GPIO);

    /* Sleep-time wakeup: the mechanism that actually ends an automatic or
     * explicit light sleep on this pin. GPIO wakeup (not EXT0/EXT1) because
     * it works on any pin and composes with the matrix rows' own GPIO
     * wakeup sources (matrix_arm_key_wake) via the same esp_sleep_enable_
     * gpio_wakeup() call — ESP-IDF "Sleep Modes", GPIO Wakeup (Light-sleep
     * Only): the only wakeup source usable on ANY pin, RTC or not. The USB
     * OTG controller itself is NOT in that list (that is the root cause —
     * see usb_wake_guard.h), which is why this pin-watching exists at all. */
    gpio_wakeup_enable(USB_DP_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    ESP_LOGI(TAG, "usb wake guard armed on D+ (GPIO%d)", (int)USB_DP_GPIO);
}

void usb_wake_guard_on_mount(void)
{
    if (usb_wake_guard_mounted(&s_guard)) {
        if (s_lock) esp_pm_lock_release(s_lock);
        ESP_LOGI(TAG, "usb: mounted, releasing the enumeration light-sleep hold");
    }
}

/* Called from veille_task's ~1 Hz tick — see usb_wake_guard.h for why 1 Hz
 * is enough here (this is a safety net, not the reaction path: the ISR
 * already acquired the lock synchronously). Also carries the bench log line
 * for the reaction path itself, at this same cheap cadence rather than from
 * the ISR (ESP_LOGx is not ISR-safe). */
void usb_wake_guard_tick(uint32_t now_ms)
{
    if (s_log_pending) {
        s_log_pending = false;
        ESP_LOGI(TAG, "usb: bus activity, holding light sleep off until mount");
    }
    if (usb_wake_guard_timeout(&s_guard, now_ms)) {
        if (s_lock) esp_pm_lock_release(s_lock);
        ESP_LOGW(TAG, "usb: no mount within %u ms of bus activity, releasing the hold",
                 (unsigned)USB_WAKE_GUARD_TIMEOUT_MS);
    }
}

#else
void usb_wake_guard_init(void) {}
void usb_wake_guard_on_mount(void) {}
void usb_wake_guard_tick(uint32_t now_ms) { (void)now_ms; }
#endif
