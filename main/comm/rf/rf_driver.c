/*
 * NRF24L01+ driver for the dongle RX path.
 * One instance per physical radio; both share an SPI bus (manual CSN).
 */
#include "rf_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "rf_drv";

/* NRF24L01+ commands */
#define CMD_R_REGISTER(r)  (0x00 | ((r) & 0x1F))
#define CMD_W_REGISTER(r)  (0x20 | ((r) & 0x1F))
#define CMD_R_RX_PAYLOAD   0x61
#define CMD_R_RX_PL_WID    0x60
#define CMD_FLUSH_RX       0xE2
#define CMD_NOP            0xFF

/* Registers */
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RX_ADDR_P0  0x0A
#define REG_RX_PW_P0    0x11
#define REG_FIFO_STATUS 0x17
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D

static inline void csn_low(rf_radio_t *r)  { gpio_set_level(r->cfg.pin_csn, 0); }
static inline void csn_high(rf_radio_t *r) { gpio_set_level(r->cfg.pin_csn, 1); }
static inline void ce_low(rf_radio_t *r)   { gpio_set_level(r->cfg.pin_ce, 0); }
static inline void ce_high(rf_radio_t *r)  { gpio_set_level(r->cfg.pin_ce, 1); }

/* Full-duplex byte exchange over an already-asserted CSN. */
static void spi_xfer(rf_radio_t *r, const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = {0};
    t.length = n * 8;
    t.tx_buffer = tx;
    t.rxlength = n * 8;
    t.rx_buffer = rx;
    ESP_ERROR_CHECK(spi_device_polling_transmit(r->spi, &t));
}

uint8_t rf_driver_read_reg(rf_radio_t *r, uint8_t reg)
{
    uint8_t tx[2] = { CMD_R_REGISTER(reg), CMD_NOP };
    uint8_t rx[2] = {0};
    csn_low(r); spi_xfer(r, tx, rx, 2); csn_high(r);
    return rx[1];
}

void rf_driver_write_reg(rf_radio_t *r, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { CMD_W_REGISTER(reg), val };
    uint8_t rx[2] = {0};
    csn_low(r); spi_xfer(r, tx, rx, 2); csn_high(r);
}

static void write_reg_buf(rf_radio_t *r, uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[6], rx[6];
    tx[0] = CMD_W_REGISTER(reg);
    memcpy(&tx[1], buf, n);
    csn_low(r); spi_xfer(r, tx, rx, n + 1); csn_high(r);
}

void rf_driver_set_channel(rf_radio_t *r, uint8_t ch)
{
    rf_driver_write_reg(r, REG_RF_CH, ch & 0x7F);
}

bool rf_driver_probe(rf_radio_t *r)
{
    uint8_t cfg = rf_driver_read_reg(r, REG_CONFIG);
    uint8_t rfs = rf_driver_read_reg(r, REG_RF_SETUP);
    bool ok = !((cfg == 0x00 && rfs == 0x00) || (cfg == 0xFF && rfs == 0xFF));
    ESP_LOGI(TAG, "probe csn=%d: CONFIG=0x%02x RF_SETUP=0x%02x -> %s",
             r->cfg.pin_csn, cfg, rfs, ok ? "OK" : "ABSENT");
    return ok;
}

void rf_radio_set_irq_sem(rf_radio_t *r, void *sem) { r->irq_sem = sem; }

esp_err_t rf_driver_init(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;

    /* CSN + CE as GPIO outputs */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->pin_csn) | (1ULL << cfg->pin_ce),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    csn_high(r);
    ce_low(r);

    /* IRQ pin: input with pull-up (NRF IRQ is active-low) */
    gpio_config_t irq_io = {
        .pin_bit_mask = (1ULL << cfg->pin_irq),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,  /* ISR attached later by rf_rx_task */
    };
    gpio_config(&irq_io);

    /* SPI bus (only the first radio initializes it) */
    if (cfg->shares_bus_first) {
        spi_bus_config_t bus = {
            .mosi_io_num = cfg->pin_mosi,
            .miso_io_num = cfg->pin_miso,
            .sclk_io_num = cfg->pin_sck,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 64,
        };
        esp_err_t e = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %d", e);
            return e;
        }
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = cfg->clock_hz,
        .mode = 0,                 /* CPOL=0, CPHA=0 */
        .spics_io_num = -1,        /* manual CSN */
        .queue_size = 1,
        .command_bits = 0, .address_bits = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->spi_host, &dev, &r->spi));

    vTaskDelay(pdMS_TO_TICKS(5));  /* power-on settle */

    if (!rf_driver_probe(r)) { r->present = false; return ESP_FAIL; }

    /* Configure as PRX, 2 Mbps, ESB + DPL on pipe 0 */
    rf_driver_write_reg(r, REG_CONFIG, 0x00);          /* power down while configuring */
    rf_driver_write_reg(r, REG_EN_AA, 0x01);           /* auto-ack pipe 0 */
    rf_driver_write_reg(r, REG_EN_RXADDR, 0x01);       /* enable pipe 0 */
    rf_driver_write_reg(r, REG_SETUP_AW, 0x03);        /* 5-byte address */
    rf_driver_write_reg(r, REG_SETUP_RETR, 0x13);      /* ARD=500us, ARC=3 */
    rf_driver_set_channel(r, cfg->channel);
    rf_driver_write_reg(r, REG_RF_SETUP, 0x0E);        /* 2 Mbps, 0 dBm */
    rf_driver_write_reg(r, REG_FEATURE, 0x04);         /* EN_DPL */
    rf_driver_write_reg(r, REG_DYNPD, 0x01);           /* dynamic payload pipe 0 */

    uint8_t addr[5];
    memcpy(addr, cfg->rx_addr, 4);
    addr[4] = cfg->addr_suffix;
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);

    /* Clear status flags, flush RX */
    rf_driver_write_reg(r, REG_STATUS, 0x70);          /* clear RX_DR|TX_DS|MAX_RT */
    { uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }

    /* Power up as PRX: EN_CRC|CRCO|PWR_UP|PRIM_RX, mask TX_DS + MAX_RT IRQs.
     * RX_DR IRQ stays enabled (bit6 MASK_RX_DR = 0). */
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);
    vTaskDelay(pdMS_TO_TICKS(2));
    ce_high(r);                                        /* start listening */

    r->present = true;
    ESP_LOGI(TAG, "radio ch=%u addr=KaSe.%02x init OK", cfg->channel, cfg->addr_suffix);
    return ESP_OK;
}

bool rf_driver_rx_available(rf_radio_t *r)
{
    uint8_t fifo = rf_driver_read_reg(r, REG_FIFO_STATUS);
    return (fifo & 0x01) == 0;   /* bit0 RX_EMPTY = 0 -> data present */
}

uint16_t rf_driver_read_rx(rf_radio_t *r, uint8_t *buf, uint16_t maxlen)
{
    if (!rf_driver_rx_available(r)) return 0;

    /* Read payload width (DPL) */
    uint8_t twx[2] = { CMD_R_RX_PL_WID, CMD_NOP };
    uint8_t trx[2] = {0};
    csn_low(r); spi_xfer(r, twx, trx, 2); csn_high(r);
    uint8_t w = trx[1];
    if (w == 0 || w > 32) {       /* corrupt: flush */
        uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r);
        rf_driver_write_reg(r, REG_STATUS, 0x40);
        r->fifo_ovf++;
        return 0;
    }
    if (w > maxlen) w = maxlen;

    uint8_t tx[33], rx[33];
    tx[0] = CMD_R_RX_PAYLOAD;
    memset(&tx[1], CMD_NOP, w);
    csn_low(r); spi_xfer(r, tx, rx, w + 1); csn_high(r);
    memcpy(buf, &rx[1], w);

    rf_driver_write_reg(r, REG_STATUS, 0x40);   /* clear RX_DR */
    r->pkt_rx++;
    return w;
}
