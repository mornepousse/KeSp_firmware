/* OLED I2C (SSD1306) backend implementation — thin driver.
 *
 * All the UI (status card, tama, layer label, KPM bar, indicators) now
 * lives in the multi-screen manager: oled_nav (state machine),
 * oled_screens (registry + build/destroy/update), oled_kpm (KPM window) and
 * the screens/screen_*.c modules. This backend now only:
 *   - configures the hardware (oled_init);
 *   - drives the manager under the LVGL lock (refresh/tick/layer);
 *   - forwards events (keypress/mouse/disp_key/activity);
 *   - handles sleep/wake (power panel) and the DFU screen.
 */
#include "display_backend.h"
#include "status_display.h"
#include "board.h"
#include "i2c_oled_display.h"
#include "oled_nav.h"
#include "oled_screens.h"
#include "oled_kpm.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_FONT_DECLARE(lv_font_montserrat_28);

/* Millisecond clock derived from the FreeRTOS tick (base shared with the manager). */
static uint32_t now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

/* true once a manager reset has built the nav; keeps oled_update()
 * from running before the first refresh_all. */
static bool oled_initialized = false;

/* true from the very first refresh_all onward (= real boot). Used to arm the
 * splash only at startup: wake also goes through refresh_all but must NOT
 * re-trigger the splash. Never reset to false (sleep touches oled_initialized,
 * not this one). */
static bool oled_booted = false;

/* ── Backend interface ───────────────────────────────────────────── */

static bool oled_init(void)
{
    display_hw_config_t cfg = {
        .bus_type       = BOARD_DISPLAY_BUS,
        .width          = BOARD_DISPLAY_WIDTH,
        .height         = BOARD_DISPLAY_HEIGHT,
        .pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
        .reset_pin      = BOARD_DISPLAY_RESET,
        .i2c = {
            .host                   = BOARD_DISPLAY_I2C_HOST,
            .sda                    = BOARD_DISPLAY_I2C_SDA,
            .scl                    = BOARD_DISPLAY_I2C_SCL,
            .address                = BOARD_DISPLAY_I2C_ADDR,
            .enable_internal_pullups = BOARD_DISPLAY_I2C_PULLUPS,
        },
    };
    display_set_hw_config(&cfg);
    init_display();
    return display_available;
}

/* Full (re)build: clean slate + manager reset. oled_screens_reset() resets the
 * KPM to zero and re-syncs tama_enabled (but NO LONGER arms the splash). The
 * splash is only armed on the very first call (real boot), via oled_screens_boot();
 * subsequent refreshes (wake, BT action...) do not show a splash. LVGL lock
 * held (the manager touches LVGL objects via destroy()). */
static void oled_refresh_all(void)
{
    if (!display_available) return;
    if (!lvgl_port_lock(200)) return;
    uint32_t t = now_ms();
    oled_screens_reset(t);
    if (!oled_booted) { oled_screens_boot(t); oled_booted = true; }  /* splash on boot only */
    display_clear_screen();
    oled_initialized = true;
    lvgl_port_unlock();
}

/* Periodic tick: the manager advances the KPM, resolves the active screen,
 * builds/cleans/destroys as needed then update(). oled_screens_tick() requires the lock held. */
static void oled_update(void)
{
    if (!display_available || !oled_initialized) return;
    if (!lvgl_port_lock(50)) return;
    oled_screens_tick(now_ms());
    lvgl_port_unlock();
}

/* Layer change: counts as activity (no screen switch — HOME
 * already shows the layer) then forces a tick to refresh HOME right away. */
static void oled_update_layer(void)
{
    if (!display_available) return;
    if (!oled_initialized) { oled_refresh_all(); return; }
    oled_screens_layer_changed(now_ms());
    oled_update();   /* refreshes HOME immediately with the new layer */
}

static void oled_sleep(void)
{
    if (!lvgl_port_lock(100)) return;
    /* Destroys the current screen via the manager (reset -> destroy + nav_init) and
     * blanks the LVGL buffer. */
    oled_screens_reset(now_ms());
    display_clear_screen();
    oled_initialized = false;
    /* Powers the panel OFF (SSD1306 display-off) while holding the LVGL lock
     * (no concurrent I2C flush). The clear only blanks the LVGL buffer;
     * in light sleep the LVGL task is frozen and never flushes, so without
     * this the last frame would stay frozen on the screen. */
    i2c_oled_display_power(false);
    lvgl_port_unlock();
}

static void oled_wake(void)
{
    i2c_oled_display_power(true);   /* panel back on */
    request_wake_request = true;    /* triggers a refresh_all -> reset + splash */
}

static void oled_notify_mouse(void)
{
    /* The old "M" mouse indicator on HOME was removed in the rewrite;
     * only the activity (idle-tama wake) is forwarded now. */
    oled_screens_activity(now_ms());
}

static void oled_notify_keypress(void)
{
    oled_kpm_notify_keypress();
    oled_screens_activity(now_ms());
}

static void oled_notify_display_key(void)
{
    oled_screens_disp_key(now_ms());
}

static void oled_show_dfu(void)
{
    /* display_clear_screen() does an lv_obj_clean (LVGL write) -> must be
     * UNDER the lock, like the other backend paths (lock consistency). */
    if (lvgl_port_lock(0)) {
        display_clear_screen();
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_label_set_text(label, "DFU");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lvgl_port_unlock();
    }
}

const display_backend_t oled_display_backend = {
    .init               = oled_init,
    .update_layer       = oled_update_layer,
    .update             = oled_update,
    .refresh_all        = oled_refresh_all,
    .sleep              = oled_sleep,
    .wake               = oled_wake,
    .notify_mouse       = oled_notify_mouse,
    .notify_keypress    = oled_notify_keypress,
    .notify_display_key = oled_notify_display_key,
    .show_dfu           = oled_show_dfu,
};
