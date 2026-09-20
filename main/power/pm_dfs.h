#pragma once
#include <stdbool.h>
/* Dynamic frequency scaling (DFS) of the Niphargus halves — see pm_dfs.c.
 * Does nothing if CONFIG_PM_ENABLE is not active. */
void pm_dfs_init(void);
/* Call on TINYUSB_EVENT_ATTACHED / DETACHED (usb_hid.c): APB lock
 * held as long as a host is mounted, USB needs the PLL. */
void pm_dfs_usb_event(bool monte);
