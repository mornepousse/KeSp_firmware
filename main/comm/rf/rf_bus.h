#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

/* Le bus SPI d'une moitié appartient à la RADIO — « une puce, un propriétaire »
 * (CLAUDE.md : deux modules qui touchaient la puce chacun de leur côté ont
 * cassé le lien trois fois, en silence). Un autre esclave sur ce bus — l'écran
 * Sharp memory-LCD — n'y accède que sous le verrou du propriétaire, donc jamais
 * pendant un envoi nRF. Implémenté UNE fois par moitié : kbd_relay (gauche) ou
 * half_link (droite). Timeout court : si la radio est occupée, l'écran cède et
 * réessaie au tick suivant — une frappe vaut plus qu'un pixel.
 *
 * ⚠ Ne jamais appeler depuis le chemin d'émission radio (il tient déjà ce
 * verrou, non récursif) ni depuis une ISR. */
bool rf_bus_lock(uint32_t timeout_ms);
void rf_bus_unlock(void);
spi_host_device_t rf_bus_host(void);
