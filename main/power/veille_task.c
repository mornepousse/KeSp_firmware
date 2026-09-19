/* Une seule tâche de veille pour les deux moitiés — voir veille_task.h.
 *
 * Elle tourne à VEILLE_TICK_MS (1 s) : la veille n'arrive qu'à 15 s
 * d'inactivité, une seconde de latence ne se voit pas, et à 10 ms cette
 * évaluation sortait le processeur d'oisiveté cent fois par seconde. Un veto
 * posé après une frappe ne retarde rien ; seul le battement de coeur attend
 * le tick. veille_pas() peut bloquer des heures : c'est cette tâche qui porte
 * le light sleep (esp_light_sleep_start) et le réveil. */
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
#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD
#include "usb_presence.h"   /* rattrapage du veto USB : usb_presence_cable() */
#endif
#include <stdio.h>

static const char *TAG = "veille";
static veille_vetos_t s_vetos;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static veille_hook_t  s_hooks[VEILLE_HOOKS_MAX];
static int            s_n_hooks;

const char *__attribute__((weak)) veille_hb_suffixe(void) { return ""; }

void veille_hook_enregistrer(const veille_hook_t *h)
{
    if (s_n_hooks < VEILLE_HOOKS_MAX) { s_hooks[s_n_hooks++] = *h; return; }
    ESP_LOGE(TAG, "trop de hooks : %s ignore (VEILLE_HOOKS_MAX=%d)", h->nom, VEILLE_HOOKS_MAX);
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

/* Battement de coeur : seul témoin de vie sur batterie (l'USB ne dit plus
 * rien). « dormi X s/n » lit une nuit d'un coup d'oeil — une nuit à 0,2 V
 * perdus est indiscernable d'une nuit à 244 µA sans ce chiffre. */
static void hb(uint32_t inactif_ms, const veille_vetos_t *v)
{
    uint32_t dodo_n = 0, dodo_ms = 0; char vb[24];
    veille_bilan(&dodo_n, &dodo_ms);
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout);   /* banc : light_sleep_counts, temps par mode, verrous */
    esp_timer_dump(stdout);      /* banc : qui arme des alarmes trop rapprochées */
#endif
    ESP_LOGW(TAG, "HB up=%lus inactif=%lus dormi=%lus/%lu vetos=%s%s",
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
        /* Rattrapage : TinyUSB ne signale pas toujours le débranchement à chaud
         * sur l'ESP32-S3 (mounted reste vrai). tud_ready() retombe dès que le
         * bus se suspend — c'est le signal qu'utilise déjà le routage USB/RF.
         * Contrepartie assumée : un hôte qui s'endort câble branché laisse
         * aussi le clavier dormir ; il se ré-énumère au réveil. */
        veille_veto(VEILLE_VETO_USB, usb_presence_cable());   /* pont VBUS si soudé, sinon tud_ready */
#endif
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t inactif = now - get_last_activity_time_ms();
        veille_vetos_t v = vetos_lire();
        if ((uint32_t)(now - dernier_hb) >= HB_PERIODE_MS) { dernier_hb = now; hb(inactif, &v); }
        if (veille_bloquee(&v) && inactif >= (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u
            && (uint32_t)(now - dernier_refus) >= 30000u) {
            char vb[24]; dernier_refus = now;
            ESP_LOGW(TAG, "veille REFUSEE depuis %lu s : vetos=%s",
                     (unsigned long)(inactif / 1000), veille_vetos_str(&v, vb, sizeof vb));
        }
        veille_pas(inactif, veille_bloquee(&v));   /* peut bloquer des heures (light sleep) */
        vTaskDelay(pdMS_TO_TICKS(VEILLE_TICK_MS));
    }
}

void veille_task_start(void)
{
    xTaskCreatePinnedToCore(veille_task, "veille", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "tache de veille : tick %u ms, %d hook(s)", (unsigned)VEILLE_TICK_MS, s_n_hooks);
}
