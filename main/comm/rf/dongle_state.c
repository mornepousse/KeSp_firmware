/*
 * État du dongle — ce qu'il lui reste à porter.
 *
 * Ce fichier s'appelait dongle_engine_state.c et fabriquait les globales que
 * matrix_scan.c fournit sur un clavier : MATRIX_STATE, keycodes[],
 * current_press_*, stat_matrix_changed. Il n'existait que pour faire tourner le
 * moteur keymap sur des demi-matrices reçues par radio — l'architecture A.
 *
 * Le Niphargus envoie du HID déjà fini, donc le dongle ne décode plus aucune
 * matrice et ne fait plus tourner de moteur. Voir
 * docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md
 *
 * Ne restent ici que trois choses qui ont encore un sens sur un répéteur :
 * le mode de sortie HID, la couche courante rapportée au moniteur, et le cache
 * des batteries des deux slots — la part « il sait et il rapporte » de son rôle.
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "sdkconfig.h"   /* CONFIG_KASE_DONGLE_FUSION : le dongle retrouve le moteur */

/* Mode de sortie HID (0 = USB). Le dongle n'a pas de BLE, mais hid_transport.c
 * lit cette globale sur tous les rôles. */
uint8_t usb_bl_state = 0;

/* Couche courante — rapportée telle quelle au moniteur CDC. Hors fusion le
 * dongle n'a pas de moteur, elle ne change jamais de son fait ; sous fusion le
 * moteur (key_processor) l'écrit, mais sa DÉFINITION vit ici de toute façon :
 * sur un clavier elle est dans matrix_scan.c, non compilé au dongle (pas de
 * matrice locale). Inconditionnelle, donc. */
uint8_t current_layout = 0;

/* ── Symboles d'état moteur, mode FUSION ──────────────────────────────────
 * Sous KASE_DONGLE_FUSION le dongle fait tourner le moteur keymap. Il compile
 * donc le bloc CLAVIER du protocole CDC, qui référence des globales que
 * matrix_scan.c fournit sur un clavier — or il n'a pas de matrice locale.
 * On les fournit ici, comme cdc_niphar_slave_stubs.c le fait pour la droite.
 * matrix_test_* : la commande de test de matrice est inerte sans matrice.
 * layer_changed : pas d'écran à notifier sur ce rôle (pour l'instant). */
#if CONFIG_KASE_DONGLE_FUSION
volatile bool     matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
void layer_changed(void) { /* pas d'écran ni de lien à notifier ici (phase 1) */ }
#endif

/* ── Cache des batteries des deux slots ───────────────────────────────────
 * Indexé comme les slots RF : 0 = clavier, 1 = souris (comm/rf/rf_slot.h).
 * Alimenté par la trame d'état reçue de chaque appareil, lu par la commande CDC
 * BATTERY. C'est la supervision : le dongle sait et rapporte, il ne décide rien.
 *
 * La trame d'état ne porte que la tension — quatre octets était une contrainte
 * de conception, pas un oubli. L'état de charge et la charge en cours restent
 * donc à 0xFF « inconnu » plutôt que devinés. */
typedef struct {
    uint8_t  batt_dV;     /* 0xFF = inconnu */
    uint8_t  soc_pct;     /* 0xFF = inconnu */
    uint8_t  charging;    /* 0xFF = inconnu */
    uint32_t last_ms;     /* 0 = jamais vu */
} dongle_batt_t;

static dongle_batt_t s_batt[2] = {
    { 0xFF, 0xFF, 0xFF, 0 },
    { 0xFF, 0xFF, 0xFF, 0 },
};

void dongle_cache_set_battery(uint8_t slot,
                              uint8_t batt_dV, uint8_t soc_pct, uint8_t charging)
{
    if (slot > 1) return;
    s_batt[slot].batt_dV  = batt_dV;
    s_batt[slot].soc_pct  = soc_pct;
    s_batt[slot].charging = charging;
    s_batt[slot].last_ms  = (uint32_t)(esp_timer_get_time() / 1000);
}

void dongle_cache_get_battery(uint8_t slot,
                              uint8_t *batt_dV, uint8_t *soc_pct,
                              uint8_t *charging, uint32_t *age_ms_out)
{
    uint8_t s = (slot > 1) ? 1 : slot;
    *batt_dV  = s_batt[s].batt_dV;
    *soc_pct  = s_batt[s].soc_pct;
    *charging = s_batt[s].charging;
    if (s_batt[s].last_ms == 0) {
        *age_ms_out = 0xFFFFFFFFu;
    } else {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        *age_ms_out = now - s_batt[s].last_ms;
    }
}

/* ── Champs du moniteur CDC ───────────────────────────────────────────────
 * Le dongle ne compte aucune frappe : il relaie du HID déjà fini, sans jamais
 * regarder ce qu'il contient. Ces deux valeurs restent exposées, à zéro, plutôt
 * que d'amputer le format de trame du moniteur — le logiciel de contrôle lit
 * ainsi les mêmes champs quel que soit l'appareil au bout du câble.
 *
 * Sous FUSION le moteur est là pour de vrai : key_stats.c définit
 * key_stats_total et key_features.c définit wpm_get(). On ne les bouchonne donc
 * qu'en l'absence de moteur, sinon le lien aurait une double définition. */
#if !CONFIG_KASE_DONGLE_FUSION
uint32_t key_stats_total = 0;

uint16_t wpm_get(void) { return 0; }
#endif
