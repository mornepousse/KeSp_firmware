/* Voir memlcd_panel.h. Protocole Sharp memory-LCD (famille LS0xx) :
 *   write  : [M0=1 | VCOM] [adresse ligne 1..N] [pixels] [0x00]  (répétable) [0x00]
 *   vcom   : [VCOM] [0x00]
 *   clear  : [M2=1 | VCOM] [0x00]
 * Le panneau lit LSB-first : commande et adresse passent par memlcd_rev8. Les
 * pixels sont posés bit 0 = pixel 0 dans le tampon, donc envoyés tels quels.
 * ⚠ La datasheet du LS011B7DH03 n'est pas dans la bibliothèque locale : la
 * géométrie (MEMLCD_LINE_BYTES, ordre des lignes, polarité) est PROUVÉE par le
 * damier de bring-up, pas supposée. */
#include "memlcd_panel.h"
#include "board.h"
#include "rf_bus.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "memlcd";

#define CMD_WRITE 0x80   /* M0 */
#define CMD_VCOM  0x40   /* M1 */
#define CMD_CLEAR 0x20   /* M2 */
#define LINES_PER_XFER 16
#define SPI_HZ 1000000   /* 1 MHz : marge sous les 2 MHz du panneau, câble breakout */

static uint8_t s_no_rev8;   /* 1 = commandes/adresses envoyées brutes */
static inline uint8_t cmd8(uint8_t v) { return s_no_rev8 ? v : memlcd_rev8(v); }
static spi_device_handle_t s_dev;
static uint8_t s_vcom;                       /* 0 ou CMD_VCOM, basculé à chaque trame */
static uint8_t s_buf[2 + (MEMLCD_LINE_BYTES + 2) * LINES_PER_XFER];

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
    gpio_set_direction(BOARD_LCD_CS_GPIO, GPIO_MODE_INPUT_OUTPUT);   /* relisible au banc */
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
    ESP_LOGI(TAG, "panneau LS011B7DH03 : %dx%d portrait, CS GPIO%d actif haut, bus partage nRF24",
             MEMLCD_W, MEMLCD_H, BOARD_LCD_CS_GPIO);
    return ESP_OK;
}

bool memlcd_panel_clear(void)
{
    if (!s_dev || !rf_bus_lock(5)) return false;
    s_vcom ^= CMD_VCOM;
    uint8_t b[2] = { cmd8((uint8_t)(CMD_CLEAR | s_vcom)), 0x00 };
    bool ok = xfer(b, 2);
    rf_bus_unlock();
    return ok;
}

bool memlcd_panel_vcom_tick(void)
{
    if (!s_dev || !rf_bus_lock(5)) return false;
    s_vcom ^= CMD_VCOM;
    uint8_t b[2] = { cmd8(s_vcom), 0x00 };
    bool ok = xfer(b, 2);
    rf_bus_unlock();
    return ok;
}

bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *bits)
{
    if (!s_dev || !bits || first + count > MEMLCD_H) return false;
    if (!rf_bus_lock(5)) return false;               /* radio occupée : on cède, tick suivant */
    s_vcom ^= CMD_VCOM;
    bool ok = true;
    for (uint16_t done = 0; done < count && ok; done += LINES_PER_XFER) {
        uint16_t n = (uint16_t)((count - done > LINES_PER_XFER) ? LINES_PER_XFER : (count - done));
        size_t p = 0;
        s_buf[p++] = cmd8((uint8_t)(CMD_WRITE | s_vcom));
        for (uint16_t i = 0; i < n; i++) {
            uint16_t ligne = (uint16_t)(first + done + i);
#if BOARD_LCD_ROTATE_180
            ligne = (uint16_t)(MEMLCD_H - 1 - ligne);
#endif
            s_buf[p++] = cmd8((uint8_t)(ligne + 1));                  /* adresses 1..160 */
            memcpy(&s_buf[p], bits + (size_t)(done + i) * MEMLCD_LINE_BYTES, MEMLCD_LINE_BYTES);
            p += MEMLCD_LINE_BYTES;
            s_buf[p++] = 0x00;                                         /* dummy fin de ligne */
        }
        s_buf[p++] = 0x00;                                             /* dummy fin de trame */
        ok = xfer(s_buf, p);
    }
    rf_bus_unlock();
    return ok;
}

/* BANC — balayage d'hypothèses. Le panneau ne répond à rien : avant de
 * conclure au matériel, on lui parle de 4 façons, 4 s chacune, en annonçant le
 * numéro au journal. L'utilisateur dit à quel numéro l'écran réagit.
 *   1 : rev8 (LSB-first émulé), pixels 0xAA/0x55         (l'hypothèse actuelle)
 *   2 : SANS rev8 (MSB-first brut), pixels 0xAA/0x55
 *   3 : rev8, tout NOIR (0x00) puis tout BLANC (0xFF) — polarité
 *   4 : SANS rev8, tout NOIR puis tout BLANC
 * Un cadre ou une teinte qui change = le protocole parle ; rien nulle part =
 * physique (alim/câblage J12). */

void memlcd_panel_sweep(void)
{
    static uint8_t bits[MEMLCD_H * MEMLCD_LINE_BYTES];
    for (int hyp = 1; hyp <= 4; hyp++) {
        s_no_rev8 = (hyp == 2 || hyp == 4);
        if (hyp <= 2) {
            for (int y = 0; y < MEMLCD_H; y++)
                memset(&bits[y * MEMLCD_LINE_BYTES], ((y / 8) & 1) ? 0x55 : 0xAA, MEMLCD_LINE_BYTES);
            ESP_LOGW(TAG, "SWEEP %d : %s, damier", hyp, s_no_rev8 ? "MSB brut" : "rev8");
            memlcd_panel_clear(); memlcd_panel_write_lines(0, MEMLCD_H, bits);
            vTaskDelay(pdMS_TO_TICKS(4000));
        } else {
            ESP_LOGW(TAG, "SWEEP %d : %s, tout 0x00 (2 s) puis tout 0xFF (2 s)", hyp, s_no_rev8 ? "MSB brut" : "rev8");
            memset(bits, 0x00, sizeof bits); memlcd_panel_write_lines(0, MEMLCD_H, bits);
            vTaskDelay(pdMS_TO_TICKS(2000));
            memset(bits, 0xFF, sizeof bits); memlcd_panel_write_lines(0, MEMLCD_H, bits);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    s_no_rev8 = 0;
    ESP_LOGW(TAG, "SWEEP fini — retour hypothèse 1");
}

void memlcd_panel_test_pattern(void)
{
    /* BANC : le CS bouge-t-il vraiment ? On le lève, on le relit, on le baisse.
     * Si la relecture ne suit pas, GPIO14 est mal configuré ou pris ailleurs —
     * et l'écran n'est jamais sélectionné, quoi qu'on envoie. */
    gpio_set_level(BOARD_LCD_CS_GPIO, 1); esp_rom_delay_us(5);
    int haut = gpio_get_level(BOARD_LCD_CS_GPIO);
    gpio_set_level(BOARD_LCD_CS_GPIO, 0); esp_rom_delay_us(5);
    int bas = gpio_get_level(BOARD_LCD_CS_GPIO);
    ESP_LOGW(TAG, "CS GPIO%d relu : haut=%d bas=%d (attendu 1/0)", BOARD_LCD_CS_GPIO, haut, bas);

    /* Damier 8 px : blocs de 8 lignes alternant 0xAA/0x55 par octet → cases de
     * 8×8. Net = géométrie et LSB/MSB corrects ; décalé/brouillé = un paramètre
     * à revoir (un seul à la fois : MEMLCD_LINE_BYTES, polarité, rotation). */
    static uint8_t bits[MEMLCD_H * MEMLCD_LINE_BYTES];
    for (int y = 0; y < MEMLCD_H; y++) {
        uint8_t v = ((y / 8) & 1) ? 0x55 : 0xAA;
        memset(&bits[y * MEMLCD_LINE_BYTES], v, MEMLCD_LINE_BYTES);
    }
    bool ok = memlcd_panel_write_lines(0, MEMLCD_H, bits);
    ESP_LOGW(TAG, "damier %s", ok ? "ecrit" : "REFUSE (bus occupe ou panneau absent)");
}
