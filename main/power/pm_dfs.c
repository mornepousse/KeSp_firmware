/* Fréquence dynamique (DFS) — ESP-IDF power management.
 *
 * Le coût du Niphargus n'est pas la veille (244 µA) mais l'ÉVEIL OISIF : cœurs
 * oisifs à 160 MHz = 27,6 mA, à 40 MHz = 13,2 mA (ESP32-S3 datasheet v2.2,
 * table 5-9, p. 67 ; 42 / 19 mA en colonne typ2 avec PSRAM — le module est un
 * N16R8). esp_pm tient le processeur à la fréquence MAX tant qu'une tâche
 * tourne et le descend à MIN dès que les deux cœurs sont oisifs : la frappe ne
 * change pas, l'attente coûte moitié moins.
 *
 * Ce qu'ESP-IDF 5.5 gère seul : la console UART repasse sur XTAL
 * (esp_pm_impl_init), le SPI maître (radio, écran), le gptimer (balayage) et
 * l'ADC (jauge) prennent un verrou APB_FREQ_MAX le temps de leurs transactions.
 * Ce qu'il ne gère pas : l'UART1 du lien TRRS (source XTAL posée dans
 * link_uart.c) et l'USB OTG, qui a besoin de la PLL — à 40 MHz sur XTAL elle
 * est coupée, un hôte ne verrait pas la carte. On tient donc un verrou
 * APB_FREQ_MAX tant qu'un hôte est monté. Un branchement à froid pendant
 * l'oisiveté à 40 MHz peut ne pas énumérer : les ports USB des moitiés servent
 * à charger, la config passe par le FTDI et la radio — assumé, documenté.
 *
 * Pas de light sleep automatique ici (tickless) : les tâches périodiques à
 * 10-20 ms (rafraîchissement radio, LVGL) l'empêcheraient de toute façon ;
 * c'est le chantier « dormir entre les touches ». La veille manuelle
 * (veille.c, esp_light_sleep_start) est indépendante du DFS. */
#include "pm_dfs.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_log.h"
#include "tinyusb.h"

static const char *TAG = "pm_dfs";
static esp_pm_lock_handle_t s_usb_lock;
static bool s_usb_tenu;

/* Verrou APB tenu tant qu'un hôte est monté. Piloté par les ÉVÉNEMENTS
 * TinyUSB (montage / démontage), pas par un poll : un timer à 200 ms sortait
 * le processeur d'oisiveté cinq fois par seconde pour lire un booléen. */
static void usb_hote(bool monte)
{
    if (!s_usb_lock) return;
    if (monte && !s_usb_tenu)       { esp_pm_lock_acquire(s_usb_lock); s_usb_tenu = true;  ESP_LOGI(TAG, "hote USB monte : APB tenu a 80 MHz"); }
    else if (!monte && s_usb_tenu)  { esp_pm_lock_release(s_usb_lock); s_usb_tenu = false; ESP_LOGI(TAG, "hote USB parti : DFS libre"); }
}
/* Événement TinyUSB (tinyusb_config_t.event_cb, posé par usb_hid.c) :
 * esp_tinyusb possède tud_mount_cb/tud_umount_cb, on passe par son relais. */
void pm_dfs_usb_event(bool monte) { usb_hote(monte); }

void pm_dfs_init(void)
{
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,          /* 40 MHz : PLL coupee au repos */
        .light_sleep_enable = false,               /* veille.c s'en charge, a la main */
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_pm_configure: %s", esp_err_to_name(e)); return; }
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb_hote", &s_usb_lock));
    usb_hote(tud_mounted());   /* si l'hôte a énuméré avant nous */
    ESP_LOGW(TAG, "DFS actif : %d MHz en travail, %d MHz oisif (PLL coupee) ; hote USB => APB 80 MHz",
             cfg.max_freq_mhz, cfg.min_freq_mhz);
}
#else
void pm_dfs_init(void) {}
void pm_dfs_usb_event(bool monte) { (void)monte; }
#endif
