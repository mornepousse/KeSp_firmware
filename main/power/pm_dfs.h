#pragma once
#include <stdbool.h>
/* Fréquence dynamique (DFS) des moitiés Niphargus — voir pm_dfs.c.
 * Ne fait rien si CONFIG_PM_ENABLE n'est pas actif. */
void pm_dfs_init(void);
/* À appeler sur TINYUSB_EVENT_ATTACHED / DETACHED (usb_hid.c) : verrou APB
 * tenu tant qu'un hôte est monté, l'USB a besoin de la PLL. */
void pm_dfs_usb_event(bool monte);
