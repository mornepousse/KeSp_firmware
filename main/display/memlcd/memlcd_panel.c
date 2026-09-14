/* Voir memlcd_panel.h. Protocole Sharp memory-LCD (app note LS013B7DH03,
 * lemia doc 6845 p. 10-12, figures 6-9), tel que le SPI MSB-first de l'ESP32
 * l'émet :
 *   write  : [0x80|VCOM] ( [rev8(adresse 1..68)] [20 octets pixels] [0x00] )×n [0x00]
 *   vcom   : [VCOM] [0x00]
 *   clear  : [0x20|VCOM] [0x00]
 * Le premier bit clocké est M0 (mode), puis M1 (VCOM), M2 (clear), 5 bits
 * dummy : le mot de commande part donc BRUT (bit 7 = M0). L'adresse de ligne
 * se lit CA0 en premier (table 6 p. 10) : elle seule passe par rev8. Les
 * pixels partent D1 en premier, D = L → noir.
 * Géométrie : 68 lignes × 160 pixels (catalogue Sharp, lemia doc 6844 p. 5) ;
 * le portrait 68 × 160 est transposé par memlcd_fb_to_panel. */
#include "memlcd_panel.h"
#include "board.h"
#include "rf_bus.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "memlcd";

#define CMD_WRITE 0x80   /* M0, premier bit clocké */
#define CMD_VCOM  0x40   /* M1 */
#define CMD_CLEAR 0x20   /* M2 */
#define LINES_PER_XFER 17          /* 68 = 4 × 17 : 4 transactions par image */
#define SPI_HZ 1000000   /* 1 MHz : marge sous les 2 MHz du panneau, câble breakout */

static spi_device_handle_t s_dev;
static uint8_t s_vcom;                       /* 0 ou CMD_VCOM, basculé à chaque trame */
static uint8_t s_buf[2 + (MEMLCD_PANEL_LINE_BYTES + 2) * LINES_PER_XFER];
static uint8_t s_panel[MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES];   /* image transposée */

static inline void cs(bool on) { gpio_set_level(BOARD_LCD_CS_GPIO, on ? 1 : 0); }  /* actif HAUT */

static bool xfer(const uint8_t *tx, size_t n)
{
    spi_transaction_t t = { .length = n * 8, .tx_buffer = tx };
    cs(true);  esp_rom_delay_us(6);                  /* tsSCS : CS haut avant SCK */
    bool ok = spi_device_polling_transmit(s_dev, &t) == ESP_OK;
    esp_rom_delay_us(2);  cs(false);                 /* thSCS : SCK fini avant CS bas */
    esp_rom_delay_us(2);                              /* twSCSL : CS bas minimum */
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
    if (s_dev) return ESP_OK;   /* déjà attaché (init différée ré-appelée) */
    memlcd_cs_idle();
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,                          /* CS manuel, comme la radio */
        .queue_size = 1,
        .command_bits = 0, .address_bits = 0,
    };
    esp_err_t e = spi_bus_add_device(rf_bus_host(), &dev, &s_dev);
    if (e == ESP_ERR_INVALID_STATE) { s_dev = NULL; return e; }   /* bus pas encore créé par la radio : réessayer */
    if (e != ESP_OK) { ESP_LOGE(TAG, "spi_bus_add_device: %d", (int)e); s_dev = NULL; return e; }
    ESP_LOGI(TAG, "panneau LS011B7DH03 : %d lignes x %d px, portrait %dx%d, CS GPIO%d actif haut, bus partage nRF24",
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
    if (!rf_bus_lock(5)) return false;               /* radio occupée : on cède, tick suivant */
    s_vcom ^= CMD_VCOM;
    bool ok = true;
    for (uint16_t done = 0; done < count && ok; done += LINES_PER_XFER) {
        uint16_t n = (uint16_t)((count - done > LINES_PER_XFER) ? LINES_PER_XFER : (count - done));
        size_t p = 0;
        s_buf[p++] = (uint8_t)(CMD_WRITE | s_vcom);
        for (uint16_t i = 0; i < n; i++) {
            uint16_t ligne = (uint16_t)(first + done + i);
            s_buf[p++] = memlcd_rev8((uint8_t)(ligne + 1));           /* adresses 1..68, CA0 en premier */
            memcpy(&s_buf[p], lines + (size_t)(done + i) * MEMLCD_PANEL_LINE_BYTES, MEMLCD_PANEL_LINE_BYTES);
            p += MEMLCD_PANEL_LINE_BYTES;
            s_buf[p++] = 0x00;                                         /* 8 ck dummy après chaque ligne */
        }
        s_buf[p++] = 0x00;                                             /* 8 ck dummy de fin de trame */
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

/* Mire de bring-up, ASYMÉTRIQUE pour trancher l'orientation d'un coup d'œil :
 * cadre de 1 px, pavé plein 16 × 16 dans le coin HAUT-GAUCHE du portrait,
 * damier 8 px ailleurs. Pavé en bas ou à droite → BOARD_LCD_ROTATE_180. */
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
