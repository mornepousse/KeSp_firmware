#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "memlcd_model.h"   /* MEMLCD_W/H, MEMLCD_LINE_BYTES, memlcd_rev8 */

/* Pilote du Sharp LS011B7DH03 (module nice!view) sur le SPI PARTAGÉ avec le
 * nRF24. Toute transaction passe sous rf_bus_lock() : jamais pendant une trame
 * radio. Write-only, CS ACTIF HAUT piloté à la main, commande brute (M0 = bit 7), adresse
 * de ligne en rev8 (CA0 en premier), VCOM logiciel basculé à chaque écriture et par
 * memlcd_panel_vcom_tick() (~1 Hz éveillé ; rien en veille : image conservée).
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md §1 */

/* À appeler TRÈS tôt au boot (avant la radio) : CS en sortie et BAS, pour que
 * l'écran n'écoute jamais le trafic du bus. Idempotent, sans écran = inoffensif. */
void memlcd_cs_idle(void);

/* Ajoute le device SPI sur le bus de la radio (rf_bus_host). Après la radio. */
esp_err_t memlcd_panel_init(void);

bool memlcd_panel_clear(void);                       /* M2 : tout blanc */
/* Écrit `count` LIGNES DU PANNEAU à partir de `first` (0..67) ; lines = count × 20
 * octets déjà transposés (memlcd_fb_to_panel) : bit 7 = D1, 1 = blanc. */
bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *lines);
/* Affiche un tampon PORTRAIT (MEMLCD_H × MEMLCD_LINE_BYTES, 1 = encre) :
 * transposition + écriture des 68 lignes. ~12 ms à 1 MHz. */
bool memlcd_panel_show(const uint8_t *fb);
bool memlcd_panel_vcom_tick(void);                   /* M1 seul : entretien VCOM */
void memlcd_panel_test_pattern(void);                /* mire de bring-up : cadre + pavé haut-gauche + damier */
