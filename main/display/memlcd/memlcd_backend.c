/* Backend display_backend_t du Sharp memory-LCD — Task 3 : squelette qui prouve
 * le panneau (damier). L'UI LVGL (bandeau / couche ou logo / pied) arrive en
 * Task 4 ; ce fichier garde alors la même vtable.
 *
 * ⚠ Ordre de boot : sur la gauche « display init » précède l'init de la radio,
 * et c'est la RADIO qui crée le bus SPI (rf_driver, shares_bus_first). Ajouter
 * le device écran avant elle rend ESP_ERR_INVALID_STATE (vu au banc : écran à
 * 602 ms, radio à 652 ms). L'init est donc DIFFÉRÉE : init() ne fait que
 * réserver, et le premier update() qui trouve le bus attache le panneau. */
#include "display_backend.h"
#include "memlcd_panel.h"
#include "esp_log.h"

static const char *TAG = "memlcd_be";
static bool s_ok;          /* panneau attaché et prouvé */
static bool s_tried_ok;    /* premier attachement réussi → damier une fois */

static bool try_attach(void)
{
    if (s_ok) return true;
    if (memlcd_panel_init() != ESP_OK) return false;   /* bus pas encore là : réessai au tick suivant */
    s_ok = true;
    memlcd_panel_clear();
    memlcd_panel_test_pattern();
    ESP_LOGI(TAG, "panneau attache (bus radio pret), mire ecrite");
    return true;
}

static bool memlcd_init(void)
{
    try_attach();              /* réussit si la radio est déjà initialisée (droite) */
    return true;               /* jamais « KO » : l'attachement se fera au premier update() */
}
static void memlcd_update(void)
{
    if (!try_attach()) return;
    /* Entretien VCOM ~1 Hz : la tâche appelle update() toutes les ~100 ms. */
    static uint8_t n; if (++n >= 10) { n = 0; memlcd_panel_vcom_tick(); }
}
static void memlcd_noop(void) {}
static void memlcd_wake(void) { if (s_ok) memlcd_panel_test_pattern(); }

const display_backend_t memlcd_display_backend = {
    .init = memlcd_init, .update_layer = memlcd_noop, .update = memlcd_update,
    .refresh_all = memlcd_noop, .sleep = memlcd_noop, .wake = memlcd_wake,
    .notify_mouse = memlcd_noop, .notify_keypress = memlcd_noop,
    .notify_display_key = memlcd_noop, .show_dfu = memlcd_noop,
};
