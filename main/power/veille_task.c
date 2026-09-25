/* A single sleep task for both halves — see veille_task.h.
 *
 * It runs at VEILLE_TICK_MS (1 s): sleep only kicks in at 15 s of
 * inactivity, one second of latency is not noticeable, and at 10 ms this
 * evaluation pulled the processor out of idle a hundred times per second. A veto
 * set after a keystroke delays nothing; only the heartbeat waits for
 * the tick. veille_pas() can block for hours: it is this task that carries
 * the light sleep (esp_light_sleep_start) and the wake-up. */
#include "veille_task.h"
#include "veille.h"
#include "cadence.h"
#include "matrix_scan.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_PM_PROFILING
#include "esp_pm.h"
#endif
#include "usb_presence.h"   /* both halves: USB veto (left) and DFS lock catch-up */
#if CONFIG_PM_ENABLE
#include "pm_dfs.h"         /* pm_dfs_usb_rattrapage */
#endif
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"     /* critical battery: sleep sooner */
#endif
#include <stdio.h>

static const char *TAG = "sleep";
static veille_vetos_t s_vetos;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static veille_hook_t  s_hooks[VEILLE_HOOKS_MAX];
static int            s_n_hooks;

const char *__attribute__((weak)) veille_hb_suffixe(void) { return ""; }

void veille_hook_enregistrer(const veille_hook_t *h)
{
    if (s_n_hooks < VEILLE_HOOKS_MAX) { s_hooks[s_n_hooks++] = *h; return; }
    ESP_LOGE(TAG, "too many hooks: %s ignored (VEILLE_HOOKS_MAX=%d)", h->nom, VEILLE_HOOKS_MAX);
}
void veille_hooks_dormir(void)
{
    for (int i = 0; i < s_n_hooks; i++) if (s_hooks[i].dormir) s_hooks[i].dormir();
}
void veille_hooks_reveiller(void)
{
    for (int i = s_n_hooks - 1; i >= 0; i--) if (s_hooks[i].reveiller) s_hooks[i].reveiller();
}

void veille_veto(veille_veto_t quoi, bool on)
{
    portENTER_CRITICAL(&s_mux);
    veille_veto_poser(&s_vetos, quoi, on);
    portEXIT_CRITICAL(&s_mux);
}
static veille_vetos_t vetos_lire(void)
{
    portENTER_CRITICAL(&s_mux);
    veille_vetos_t v = s_vetos;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

/* Heartbeat: the only sign of life on battery (USB no longer says
 * anything). "slept X s/n" reads a night at a glance — a night with 0.2 V
 * lost is indistinguishable from a night at 244 µA without this figure. */
static void hb(uint32_t inactif_ms, const veille_vetos_t *v)
{
    uint32_t dodo_n = 0, dodo_ms = 0; char vb[VEILLE_VETOS_STR_MAX];
    veille_bilan(&dodo_n, &dodo_ms);
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout);   /* bench: light_sleep_counts, time per mode, locks */
    esp_timer_dump(stdout);      /* bench: who arms alarms too close together */
#endif
    ESP_LOGW(TAG, "HB up=%lus idle=%lus slept=%lus/%lu vetos=%s%s",
             (unsigned long)(esp_timer_get_time() / 1000000), (unsigned long)(inactif_ms / 1000),
             (unsigned long)(dodo_ms / 1000), (unsigned long)dodo_n,
             veille_vetos_str(v, vb, sizeof vb), veille_hb_suffixe());
}

static void veille_task(void *arg)
{
    (void)arg;
    uint32_t dernier_hb = 0, dernier_refus = 0;
    for (;;) {
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
        /* Catch-up: TinyUSB does not always report a hot unplug
         * on the ESP32-S3 (mounted stays true). tud_ready() drops as soon as the
         * bus suspends — this is the signal the USB/RF routing already uses.
         * Accepted trade-off: a host that goes to sleep with the cable plugged in
         * also lets the keyboard sleep; it re-enumerates on wake. */
        veille_veto(VEILLE_VETO_USB, usb_presence_cable());   /* VBUS bridge if soldered, else tud_ready */
#endif
#if CONFIG_PM_ENABLE
        /* Both halves: the DFS APB lock follows the same rule, or a missed
         * unmount keeps automatic light sleep out for good (pm_dfs.c). */
        pm_dfs_usb_rattrapage(usb_presence_cable());
#endif
#if CONFIG_KASE_BATT_SENSE
        /* CRITICAL battery (< 3.3 V): the light stage at 5 s instead of 15 —
         * every second of idle wakefulness counts, the cell is running out. */
        veille_seuil_legere_set(batt_sense_niveau() == 2 ? VEILLE_LEGERE_CRITIQUE_MS
                                                         : (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u);
#endif
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t inactif = now - get_last_activity_time_ms();
        veille_vetos_t v = vetos_lire();
        if ((uint32_t)(now - dernier_hb) >= HB_PERIODE_MS) { dernier_hb = now; hb(inactif, &v); }
        if (veille_bloquee(&v) && inactif >= veille_seuil_legere_ms()
            && (uint32_t)(now - dernier_refus) >= 30000u) {
            char vb[VEILLE_VETOS_STR_MAX]; dernier_refus = now;
            ESP_LOGW(TAG, "sleep REFUSED for %lu s: vetos=%s",
                     (unsigned long)(inactif / 1000), veille_vetos_str(&v, vb, sizeof vb));
        }
        veille_pas(inactif, veille_bloquee(&v));   /* can block for hours (light sleep) */
        vTaskDelay(pdMS_TO_TICKS(VEILLE_TICK_MS));
    }
}

void veille_task_start(void)
{
    xTaskCreatePinnedToCore(veille_task, "sleep", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "sleep task: tick %u ms, %d hook(s)", (unsigned)VEILLE_TICK_MS, s_n_hooks);
}
