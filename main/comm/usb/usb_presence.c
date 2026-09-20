/* USB VBUS presence reader (firmware side) — see usb_presence.h.
 * Compiled only on the wireless-relay build (CMakeLists guard). The routing
 * decision (kbd_route_target) and the debounce (vbus_debounce_step) are pure
 * header inlines, host-tested in test/test_kbd_route.c; here we only supply the
 * GPIO level and the esp_timer clock. */
#include "usb_presence.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "tinyusb.h"   /* tud_ready/tud_mounted */

#ifndef BOARD_VBUS_SENSE_GPIO
#define BOARD_VBUS_SENSE_GPIO GPIO_NUM_33   /* fallback; real value in board.h */
#endif

#define VBUS_DEBOUNCE_MS 50

static vbus_debounce_t s_db;
static kbd_out_t       s_route = KBD_OUT_USB;   /* USB-first default before first poll */

void usb_presence_init(void)
{
#if CONFIG_KASE_VBUS_SENSE
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_VBUS_SENSE_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* reads LOW if the divider is absent */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
#endif
}

bool usb_presence_active(void)
{
    /* A single rule (usb_presence_brut): VBUS bridge if soldered, otherwise
     * tud_ready() = mounted AND not suspended — tud_mounted() alone stays true
     * after a hot unplug on the S3. */
    bool raw = usb_presence_cable();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return vbus_debounce_step(&s_db, raw, now_ms, VBUS_DEBOUNCE_MS);
}

void usb_presence_poll(bool relay_active)
{
    s_route = kbd_route_target(usb_presence_active(), relay_active);
}

kbd_out_t kbd_active_route(void)
{
#if CONFIG_KASE_RF_FORCE_ROUTE
    /* Bench: HID goes out over the radio even with USB plugged in. This keeps
     * power and the USB console while we're proving the link to the
     * dongle — useful while battery power isn't reliable yet.
     * s_route is still computed by usb_presence_poll(): only the
     * decision returned here is forced, the real state stays observable. */
    return KBD_OUT_RF;
#else
    return s_route;
#endif
}

bool usb_cable_present_now(void)
{
#if CONFIG_KASE_VBUS_SENSE
    return gpio_get_level(BOARD_VBUS_SENSE_GPIO) != 0;
#else
    return tud_mounted();
#endif
}

bool usb_sleep_blocked(void)
{
#if CONFIG_KASE_VBUS_SENSE
    /* VBUS sense already distinguishes unplugged from host-asleep: route==RF only
     * when truly unplugged, so the route-based sleep gate is sufficient. */
    return false;
#else
    /* No divider: never sleep while the USB cable is enumerated (tud_mounted stays
     * true when the host suspends), so the keyboard doesn't sleep when the PC
     * sleeps. Trade-off: tud_mounted misses real unplug → no battery sleep until a
     * power cycle without USB. */
    return tud_mounted();
#endif
}
