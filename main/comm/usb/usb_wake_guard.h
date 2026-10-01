/* USB enumeration vs automatic light sleep — pure state machine.
 *
 * Root cause (bench, 2026-10-01): the ESP32-S3's built-in USB-OTG controller
 * is NOT in the documented list of light-sleep wakeup sources (ESP-IDF
 * "Sleep Modes" guide: EXT0/EXT1/GPIO/UART/timer/ULP/touch only). Unlike
 * USB_SERIAL_JTAG (which auto-holds an ESP_PM_NO_LIGHT_SLEEP lock while
 * connected, see Kconfig "ESP-Driver:USB Serial/JTAG Configuration"), the
 * OTG device stack (TinyUSB, what this firmware uses) gets no such help:
 * pm_dfs.c only acquires its APB lock on tud_mount_cb — by definition AFTER
 * enumeration, too late to let enumeration itself happen. With tickless
 * automatic light sleep on (both halves, CONFIG_FREERTOS_USE_TICKLESS_IDLE)
 * and the at-rest cadences at ~1 s (cadence.h), a half idle between
 * keystrokes can be asleep for up to ~1 s at a time; the host's bus reset /
 * SETUP packets land during one of these windows more often than not, and
 * the host has to retry for tens of seconds before the timing finally
 * lines up — matching the ~30 s enumeration delay seen on the bench.
 *
 * Fix: D+ (GPIO20 on the S3's built-in FS PHY — ESP-IDF "Establish Serial
 * Connection with ESP32-S3", confirmed against the TRM "USB 2.0 OTG
 * Full-Speed Interface") idles HIGH via the device-side pull-up and goes
 * LOW for the whole duration of a host bus reset (SE0) and for the K-state
 * portions of ordinary traffic. Arming it as a GPIO wakeup source (the one
 * documented wakeup path that works on ANY pin, RTC or not — "GPIO Wakeup
 * (Light-sleep Only)") lets a sleeping half be woken by the very first sign
 * of a host; a plain GPIO interrupt on the same pin (awake-time only, no
 * cost while asleep) then holds a dedicated ESP_PM_NO_LIGHT_SLEEP lock long
 * enough for the rest of the handshake to complete without being
 * re-swallowed by the next automatic sleep.
 *
 * This header is the pure, host-tested transition logic: activity seen,
 * mount seen, periodic timeout check. The hardware glue (GPIO ISR, the PM
 * lock itself) lives in usb_wake_guard.c and is not testable on the host.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Generous margin: a full bus-reset-to-SET_CONFIGURATION handshake is
 * normally well under 500 ms even on a slow host; this is a safety net
 * against a host that resets the bus and then goes away (no mount ever
 * follows), not a tight budget. Checked at ~1 Hz (veille_task's tick) —
 * see usb_wake_guard_timeout. */
#define USB_WAKE_GUARD_TIMEOUT_MS 2000u

typedef struct {
    bool     holding;       /* true: the PM lock is currently held */
    uint32_t deadline_ms;   /* give up and release if still holding past this */
} usb_wake_guard_t;

/* Bus activity seen (D+ went low). Cheap and safe to call every time the
 * ISR fires, including many times during one reset/packet burst: only the
 * FIRST call while not already holding needs the caller to actually acquire
 * the PM lock (that is what the return value says) — every call refreshes
 * the deadline, so a host that keeps talking never gets cut off mid-handshake. */
static inline bool usb_wake_guard_activity(usb_wake_guard_t *g, uint32_t now_ms)
{
    bool first = !g->holding;
    g->holding = true;
    g->deadline_ms = now_ms + USB_WAKE_GUARD_TIMEOUT_MS;
    return first;
}

/* Host mounted: the hold did its job. Returns true if the caller owes a
 * release (false if nothing was held — e.g. a host that was already mounted
 * before any activity was ever recorded, or a second, redundant mount event). */
static inline bool usb_wake_guard_mounted(usb_wake_guard_t *g)
{
    if (!g->holding) return false;
    g->holding = false;
    return true;
}

/* Periodic safety-net check (task context, ~1 Hz is enough — the reaction
 * to the FIRST activity is the ISR acquiring the lock directly; this only
 * catches "host went away without ever mounting"). Returns true the moment
 * the deadline passes while still holding: the caller releases the PM lock,
 * and the guard is re-armed for a future attempt (holding cleared either
 * way, so a stale deadline from a past, already-released hold is harmless). */
static inline bool usb_wake_guard_timeout(usb_wake_guard_t *g, uint32_t now_ms)
{
    if (!g->holding) return false;
    if ((int32_t)(now_ms - g->deadline_ms) < 0) return false;
    g->holding = false;
    return true;
}

#ifndef TEST_HOST
/* Firmware-only: install the D+ GPIO interrupt (awake-time activity
 * detection) and the GPIO light-sleep wakeup source, create the dedicated
 * ESP_PM_NO_LIGHT_SLEEP lock. Call once, after tinyusb_driver_install(). */
void usb_wake_guard_init(void);
/* Firmware-only: TinyUSB mount event (pm_dfs_usb_event(true)) — releases
 * the lock if usb_wake_guard_mounted() says so. */
void usb_wake_guard_on_mount(void);
/* Firmware-only: periodic tick (veille_task, ~1 Hz) — releases the lock on
 * timeout per usb_wake_guard_timeout(). */
void usb_wake_guard_tick(uint32_t now_ms);
#endif
