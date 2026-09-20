/* See memlcd_panel.h. Sharp memory-LCD protocol (app note LS013B7DH03,
 * lemia doc 6845 p. 10-12, figures 6-9), as the ESP32's MSB-first SPI
 * transmits it:
 *   write  : [0x80|VCOM] ( [rev8(address 1..68)] [20 pixel bytes] [0x00] )×n [0x00]
 *   vcom   : [VCOM] [0x00]
 *   clear  : [0x20|VCOM] [0x00]
 * The first bit clocked is M0 (mode), then M1 (VCOM), M2 (clear), 5 dummy
 * bits: the command word therefore goes out RAW (bit 7 = M0). The line address
 * is read CA0 first (table 6 p. 10): it alone goes through rev8. The
 * pixels go out D1 first, D = L → black.
 * Geometry: 68 lines × 160 pixels (Sharp catalog, lemia doc 6844 p. 5);
 * the 68 × 160 portrait is transposed by memlcd_fb_to_panel. */
#include "memlcd_panel.h"
#include "board.h"
#include "rf_bus.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "memlcd";

#define CMD_WRITE 0x80   /* M0, first bit clocked */
#define CMD_VCOM  0x40   /* M1 */
#define CMD_CLEAR 0x20   /* M2 */
#define LINES_PER_XFER 17          /* 68 = 4 × 17: 4 transactions per image */
#define SPI_HZ 1000000   /* 1 MHz: margin below the panel's 2 MHz, breakout cable */

static spi_device_handle_t s_dev;
static uint8_t s_vcom;                       /* 0 or CMD_VCOM, toggled on every frame */
static uint8_t s_buf[2 + (MEMLCD_PANEL_LINE_BYTES + 2) * LINES_PER_XFER];
static uint8_t s_panel[MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES];   /* transposed image */

static inline void cs(bool on) { gpio_set_level(BOARD_LCD_CS_GPIO, on ? 1 : 0); }  /* active HIGH */

static bool xfer(const uint8_t *tx, size_t n)
{
    spi_transaction_t t = { .length = n * 8, .tx_buffer = tx };
    cs(true);  esp_rom_delay_us(6);                  /* tsSCS: CS high before SCK */
    bool ok = spi_device_polling_transmit(s_dev, &t) == ESP_OK;
    esp_rom_delay_us(2);  cs(false);                 /* thSCS: SCK finished before CS low */
    esp_rom_delay_us(2);                              /* twSCSL: minimum CS low */
    return ok;
}

void memlcd_cs_idle(void)
{
    gpio_reset_pin(BOARD_LCD_CS_GPIO);
    gpio_set_direction(BOARD_LCD_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LCD_CS_GPIO, 0);
}

esp_err_t memlcd_panel_init(void)
{
    if (s_dev) return ESP_OK;   /* already attached (deferred init called again) */
    memlcd_cs_idle();
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,                          /* manual CS, like the radio */
        .queue_size = 1,
        .command_bits = 0, .address_bits = 0,
    };
    esp_err_t e = spi_bus_add_device(rf_bus_host(), &dev, &s_dev);
    if (e == ESP_ERR_INVALID_STATE) { s_dev = NULL; return e; }   /* bus not yet created by the radio: retry */
    if (e != ESP_OK) { ESP_LOGE(TAG, "spi_bus_add_device: %d", (int)e); s_dev = NULL; return e; }
    /* In light sleep the ESP ISOLATES its pins (sleep_gpio: "isolate all GPIO
     * pins"): CS, SCK and MOSI would float onto the panel's CMOS inputs,
     * which draw current at mid-voltage. Sleep configuration: input pulled LOW
     * (CS low = screen deselected, as at boot). Applied by itself on
     * every sleep, the internal pulls staying active (Kconfig
     * ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS). */
    const gpio_num_t dodo[] = { BOARD_LCD_CS_GPIO, BOARD_NRF_SCK, BOARD_NRF_MOSI };
    for (unsigned i = 0; i < sizeof dodo / sizeof dodo[0]; i++) {
        gpio_sleep_sel_en(dodo[i]);
        gpio_sleep_set_direction(dodo[i], GPIO_MODE_INPUT);
        gpio_sleep_set_pull_mode(dodo[i], GPIO_PULLDOWN_ONLY);
    }
    ESP_LOGI(TAG, "panneau LS011B7DH03 : %d lignes x %d px, portrait %dx%d, CS GPIO%d actif haut, bus partage nRF24, broches tirees bas en veille",
             MEMLCD_PANEL_LINES, MEMLCD_PANEL_LINE_BYTES * 8, MEMLCD_W, MEMLCD_H, BOARD_LCD_CS_GPIO);
    return ESP_OK;
}

bool memlcd_panel_clear(void)
{
    if (!s_dev || !rf_bus_lock(5)) return false;
    s_vcom ^= CMD_VCOM;
    uint8_t b[2] = { (uint8_t)(CMD_CLEAR | s_vcom), 0x00 };
    bool ok = xfer(b, 2);
    rf_bus_unlock();
    return ok;
}

bool memlcd_panel_vcom_tick(void)
{
    if (!s_dev || !rf_bus_lock(5)) return false;
    s_vcom ^= CMD_VCOM;
    uint8_t b[2] = { s_vcom, 0x00 };
    bool ok = xfer(b, 2);
    rf_bus_unlock();
    return ok;
}

bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *lines)
{
    if (!s_dev || !lines || first + count > MEMLCD_PANEL_LINES) return false;
    if (!rf_bus_lock(5)) return false;               /* radio busy: yield, next tick */
    s_vcom ^= CMD_VCOM;
    bool ok = true;
    for (uint16_t done = 0; done < count && ok; done += LINES_PER_XFER) {
        uint16_t n = (uint16_t)((count - done > LINES_PER_XFER) ? LINES_PER_XFER : (count - done));
        size_t p = 0;
        s_buf[p++] = (uint8_t)(CMD_WRITE | s_vcom);
        for (uint16_t i = 0; i < n; i++) {
            uint16_t ligne = (uint16_t)(first + done + i);
            s_buf[p++] = memlcd_rev8((uint8_t)(ligne + 1));           /* addresses 1..68, CA0 first */
            memcpy(&s_buf[p], lines + (size_t)(done + i) * MEMLCD_PANEL_LINE_BYTES, MEMLCD_PANEL_LINE_BYTES);
            p += MEMLCD_PANEL_LINE_BYTES;
            s_buf[p++] = 0x00;                                         /* 8 dummy clocks after each line */
        }
        s_buf[p++] = 0x00;                                             /* 8 dummy clocks at end of frame */
        ok = xfer(s_buf, p);
        if (!ok) ESP_LOGE(TAG, "spi_device_polling_transmit KO (transaction de %u octets)", (unsigned)p);
    }
    rf_bus_unlock();
    return ok;
}

bool memlcd_panel_show(const uint8_t *fb)
{
    if (!s_dev || !fb) return false;
    memlcd_fb_to_panel(fb, s_panel, BOARD_LCD_ROTATE_180 != 0);
    return memlcd_panel_write_lines(0, MEMLCD_PANEL_LINES, s_panel);
}

/* Bring-up test pattern, ASYMMETRIC to settle orientation at a glance:
 * 1 px frame, solid 16 × 16 block in the TOP-LEFT corner of the portrait,
 * 8 px checkerboard elsewhere. Block at bottom or right → BOARD_LCD_ROTATE_180. */
void memlcd_panel_test_pattern(void)
{
    static uint8_t fb[MEMLCD_H * MEMLCD_LINE_BYTES];
    for (int y = 0; y < MEMLCD_H; y++) {
        uint8_t *row = &fb[y * MEMLCD_LINE_BYTES];
        for (int x = 0; x < MEMLCD_W; x++) {
            bool encre = ((x / 8) + (y / 8)) & 1;
            if (x < 16 && y < 16) encre = true;
            if (x == 0 || y == 0 || x == MEMLCD_W - 1 || y == MEMLCD_H - 1) encre = true;
            if (encre) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            else       row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
    bool ok = memlcd_panel_show(fb);
    ESP_LOGW(TAG, "mire %s", ok ? "ecrite (pave plein attendu en HAUT-GAUCHE)" : "REFUSEE (bus occupe ou panneau absent)");
}
