/* Round display (GC9A01) backend implementation */
#include "display_backend.h"
#include "spi_round_display.h"
#include "round_ui.h"

static bool round_init(const display_hw_config_t *cfg)
{
    return spi_display_init(cfg);
}

static void round_update_layer(void)  { round_ui_update_layer(); }
static void round_update(void)        { round_ui_update(); }
static void round_refresh_all(void)   { round_ui_refresh_all(); }
static void round_sleep(void)         { round_ui_sleep(); }
static void round_wake(void)          { round_ui_wake(); }
static void round_notify_mouse(void)  { round_ui_notify_mouse(); }
static void round_show_dfu(void)      { round_ui_show_dfu(); }

const display_backend_t round_display_backend = {
    .init          = round_init,
    .update_layer  = round_update_layer,
    .update        = round_update,
    .refresh_all   = round_refresh_all,
    .sleep         = round_sleep,
    .wake          = round_wake,
    .notify_mouse  = round_notify_mouse,
    .show_dfu      = round_show_dfu,
};
