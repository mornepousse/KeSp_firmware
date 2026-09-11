/* Transport du lien filaire TRRS (brick B2) — voir link_uart.c. */
#pragma once
#include <stdbool.h>

/* Initialise LINK_5V_EN à BAS, l'UART1 (avec le swap si la carte l'annonce)
 * et démarre la tâche qui fait tourner la poignée de main. */
void link_uart_start(void);

/* Le 5 V est-il fermé de notre côté ? Sert de verrou à la veille : une moitié
 * en charge doit rester éveillée pour continuer à répondre aux sondes. */
bool link_uart_active(void);
