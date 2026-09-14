#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "memlcd_model.h"   /* MEMLCD_W/H, MEMLCD_LINE_BYTES, memlcd_rev8 */

/* Pilote du Sharp LS011B7DH03 (module nice!view) sur le SPI PARTAGÉ avec le
 * nRF24. Toute transaction passe sous rf_bus_lock() : jamais pendant une trame
 * radio. Write-only, CS ACTIF HAUT piloté à la main, LSB-first émulé (rev8 sur
 * commandes/adresses), VCOM logiciel basculé à chaque écriture et par
 * memlcd_panel_vcom_tick() (~1 Hz éveillé ; rien en veille : image conservée).
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md §1 */

/* À appeler TRÈS tôt au boot (avant la radio) : CS en sortie et BAS, pour que
 * l'écran n'écoute jamais le trafic du bus. Idempotent, sans écran = inoffensif. */
void memlcd_cs_idle(void);

/* Ajoute le device SPI sur le bus de la radio (rf_bus_host). Après la radio. */
esp_err_t memlcd_panel_init(void);

bool memlcd_panel_clear(void);                       /* M2 : tout blanc */
/* Écrit `count` lignes à partir de `first` (0..159) ; bits = count × 9 octets,
 * bit 0 de l'octet 0 = pixel x=0 ; polarité : 1 = BLANC (convention Sharp),
 * à confirmer au damier. */
bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *bits);
bool memlcd_panel_vcom_tick(void);                   /* M1 seul : entretien VCOM */
void memlcd_panel_test_pattern(void);                /* damier 8 px : bring-up */
