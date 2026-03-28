/* Round display (GC9A01) backend implementation */
#include "display_backend.h"
#include "spi_round_display.h"
#include "round_ui.h"
#include "board.h"

static bool round_init(void)
{
    display_hw_config_t cfg = {
        .bus_type = BOARD_DISPLAY_BUS,
        .width = BOARD_DISPLAY_WIDTH,
        .height = BOARD_DISPLAY_HEIGHT,
        .pixel_clock_hz = BOARD_DISPLAY_CLK_HZ,
        .reset_pin = BOARD_DISPLAY_RESET,
        .spi = {
            .host = BOARD_DISPLAY_SPI_HOST,
            .sclk = BOARD_DISPLAY_SPI_SCLK,
            .mosi = BOARD_DISPLAY_SPI_MOSI,
            .cs = BOARD_DISPLAY_SPI_CS,
            .dc = BOARD_DISPLAY_SPI_DC,
            .backlight = BOARD_DISPLAY_SPI_BL,
        },
    };
    return spi_display_init(&cfg);
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
