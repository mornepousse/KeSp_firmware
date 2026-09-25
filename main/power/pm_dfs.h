#pragma once
#include <stdbool.h>
/* Dynamic frequency scaling (DFS) of the Niphargus halves — see pm_dfs.c.
 * Does nothing if CONFIG_PM_ENABLE is not active. */
void pm_dfs_init(void);
/* Call on TINYUSB_EVENT_ATTACHED / DETACHED (usb_hid.c): APB lock
 * held as long as a host is mounted, USB needs the PLL. */
void pm_dfs_usb_event(bool monte);

/* Catch-up of the USB APB lock from the single USB presence rule
 * (usb_presence_cable), called every second by the sleep task: the S3 does not
 * always signal the unmount, and the lock — which forbids automatic light
 * sleep — stayed held after a hot unplug (32 mA between keystrokes, 2026-09-25). */
void pm_dfs_usb_rattrapage(bool present);
