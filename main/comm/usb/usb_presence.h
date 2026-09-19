/* USB presence (VBUS sense) + HID output routing for the wireless-relay V2D.
 *
 * TinyUSB mount/unmount detection is unreliable on the ESP32-S3 without VBUS
 * sensing, so USB-cable presence is read from a GPIO fed by a VBUS divider
 * (see boards/kase_v2_debug/board.h BOARD_VBUS_SENSE_GPIO). The routing decision
 * and the debounce are pure functions so they are host-testable; only the GPIO
 * read + esp_timer clock live in usb_presence.c (firmware-only).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* HID output path. USB is always preferred when a host is present. */
typedef enum { KBD_OUT_USB, KBD_OUT_RF, KBD_OUT_NONE } kbd_out_t;

/* Pure routing decision: USB-first. VBUS present -> USB; else relay if paired;
 * else nothing (no host reachable). */
static inline kbd_out_t kbd_route_target(bool vbus_present, bool relay_active)
{
    if (vbus_present) return KBD_OUT_USB;
    if (relay_active) return KBD_OUT_RF;
    return KBD_OUT_NONE;
}

/* Debounce: the reported "stable" level only follows the raw level after it has
 * held the new value for >= window_ms. Pure (clock + raw level are passed in) so
 * it is host-testable. Returns the (possibly updated) stable level. */
typedef struct {
    bool     stable;     /* debounced output */
    bool     cand;       /* current candidate raw level */
    uint32_t since_ms;   /* when cand was first seen */
    bool     init;       /* false until the first step seeds state */
} vbus_debounce_t;

static inline bool vbus_debounce_step(vbus_debounce_t *d, bool raw,
                                      uint32_t now_ms, uint32_t window_ms)
{
    if (!d->init) {                 /* first sample seeds both states */
        d->init = true;
        d->stable = raw;
        d->cand = raw;
        d->since_ms = now_ms;
        return d->stable;
    }
    if (raw != d->cand) {           /* candidate changed → restart the timer */
        d->cand = raw;
        d->since_ms = now_ms;
    } else if (raw != d->stable && (now_ms - d->since_ms) >= window_ms) {
        d->stable = raw;            /* held long enough → commit */
    }
    return d->stable;
}

/* Présence d'un câble USB, décision pure (test/test_kbd_route.c) : avec le pont
 * VBUS soudé (vbus_dispo), le niveau GPIO fait foi — un chargeur mural
 * n'énumère pas, un hôte peut autosuspendre ; sans pont, ce que TinyUSB voit
 * (tud_ready : mounted ET non suspendu, le seul signal qui retombe au
 * débranchement à chaud sur S3). Le forçage de banc (KASE_LINK_FORCE_SOURCE)
 * gagne sur tout. Une seule règle pour le routage, le 5 V du lien TRRS et le
 * veto de veille — jusqu'au 2026-09-19 chacun lisait sa propre source. */
static inline bool usb_presence_brut(bool vbus_dispo, bool vbus, bool tud_pret, bool force)
{
    if (force) return true;
    return vbus_dispo ? vbus : tud_pret;
}

#ifndef TEST_HOST
/* Firmware-only: configure the VBUS sense GPIO (input + pulldown). */
void usb_presence_init(void);
/* Firmware-only: debounced VBUS-present read (USB cable connected to a host). */
bool usb_presence_active(void);

/* Single route poller — called periodically (the kbd_relay 10ms refresh timer)
 * with the current relay-paired state. Updates the debounce and caches the
 * resulting route. Keeping one caller of the debounce avoids cross-thread races
 * on its state. */
void usb_presence_poll(bool relay_active);
/* Cached HID output route (cheap read for the sender + the OLED). Defaults to
 * KBD_OUT_USB until the first poll (an unpaired keyboard only has USB anyway). */
kbd_out_t kbd_active_route(void);

/* True if RF light-sleep must be suppressed because USB is present. Without a VBUS
 * divider this is tud_mounted() (don't sleep while the cable is enumerated, so the
 * keyboard stays awake when the host PC sleeps); with the divider the route gate
 * already handles it. */
bool usb_sleep_blocked(void);

/* Instantaneous "USB cable present" read (VBUS gpio, or tud_mounted without a
 * divider). Used by the light-sleep poll loop to wake on USB plug-in. */
bool usb_cable_present_now(void);

/* Présence brute (non débouncée) selon usb_presence_brut : pont VBUS si
 * KASE_VBUS_SENSE, sinon tud_ready ; forcée sous KASE_LINK_FORCE_SOURCE.
 * Inline : usb_presence.c n'est compilé que sur la gauche, le lien et la
 * veille en ont besoin sur les deux moitiés. */
#include "sdkconfig.h"
#include "tinyusb.h"
#if CONFIG_KASE_VBUS_SENSE
#include "board.h"
#include "driver/gpio.h"
#ifndef BOARD_VBUS_SENSE_GPIO
#define BOARD_VBUS_SENSE_GPIO GPIO_NUM_33
#endif
#endif
static inline bool usb_presence_cable(void)
{
    bool vbus_dispo = false, vbus = false, force = false;
#if CONFIG_KASE_VBUS_SENSE
    vbus_dispo = true; vbus = gpio_get_level(BOARD_VBUS_SENSE_GPIO) != 0;
#endif
#if CONFIG_KASE_LINK_FORCE_SOURCE
    force = true;   /* banc : source 5 V forcée, cf. Kconfig */
#endif
    return usb_presence_brut(vbus_dispo, vbus, tud_ready(), force);
}

/* usb_try_remote_wakeup() now lives in comm/usb/usb_hid.h — it must exist on
 * wired builds too, and this file is only compiled for the wireless relay. */
#endif
