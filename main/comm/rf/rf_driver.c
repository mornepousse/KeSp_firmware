/*
 * NRF24L01+ driver for the dongle RX path.
 * One instance per physical radio; both share an SPI bus (manual CSN).
 */
#include "rf_driver.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
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
#define REG_OBSERVE_TX  0x08   /* [7:4]=PLOS_CNT, [3:0]=ARC_CNT (retries last pkt) */
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

/* ── Pairing helpers (both roles) — placed after write_reg_buf ──── */
#ifndef CMD_W_TX_PAYLOAD
#define CMD_W_TX_PAYLOAD 0xA0
#endif
#ifndef CMD_FLUSH_TX
#define CMD_FLUSH_TX     0xE1
#endif
#define REG_TX_ADDR_OOB  0x10   /* avoid clashing with the RF_TX-block REG_TX_ADDR */

void rf_driver_set_rx_address(rf_radio_t *r, const uint8_t addr[5])
{
    ce_low(r);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);
    ce_high(r);
}

bool rf_driver_oob_tx(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                      const uint8_t *payload, uint8_t len,
                      uint8_t restore_ch, const uint8_t restore_addr[5])
{
    /* ── Enter PTX on the pairing channel/address ── */
    ce_low(r);
    rf_driver_set_channel(r, ch);
    write_reg_buf(r, REG_TX_ADDR_OOB, addr, 5);
    write_reg_buf(r, REG_RX_ADDR_P0,  addr, 5);   /* match TX_ADDR for ESB ACK */
    rf_driver_write_reg(r, REG_CONFIG, 0x3E);     /* PTX power-up, EN_CRC|CRCO */
    vTaskDelay(pdMS_TO_TICKS(2));                  /* Tpd2stby */

    rf_driver_write_reg(r, REG_STATUS, 0x70);     /* clear flags */
    { uint8_t c = CMD_FLUSH_TX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }

    /* ── Write payload + pulse CE ── */
    uint8_t tx[33], rxb[33];
    tx[0] = CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], payload, len);
    csn_low(r); spi_xfer(r, tx, rxb, (size_t)(len + 1)); csn_high(r);
    ce_high(r); esp_rom_delay_us(15); ce_low(r);

    /* ── Poll TX_DS / MAX_RT ── */
    uint32_t deadline = (uint32_t)(esp_timer_get_time() + 5000);
    uint8_t status = 0;
    do {
        status = rf_driver_read_reg(r, REG_STATUS);
        if (status & 0x30) break;
    } while ((uint32_t)esp_timer_get_time() < deadline);
    bool ok = (status & 0x20) != 0;   /* TX_DS = ACK received */
    if (status & 0x10) { uint8_t c = CMD_FLUSH_TX, rx; csn_low(r); spi_xfer(r,&c,&rx,1); csn_high(r); }
    rf_driver_write_reg(r, REG_STATUS, 0x30);

    /* ── Restore PRX on the data address/channel ── */
    ce_low(r);
    rf_driver_set_channel(r, restore_ch);
    write_reg_buf(r, REG_RX_ADDR_P0, restore_addr, 5);
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);     /* PRX power-up */
    vTaskDelay(pdMS_TO_TICKS(2));
    rf_driver_write_reg(r, REG_STATUS, 0x70);
    { uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }
    ce_high(r);                                    /* resume listening */
    return ok;
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

/* Re-assert the PRX RX config on a live radio WITHOUT re-adding the SPI device.
 * NRF24 (clone) modules can wedge over time — stop ACKing/receiving even though
 * the SPI link is fine. Re-writing the config registers + flushing RX un-wedges
 * them. CE-low (standby) → rewrite EN_AA/RXADDR/SETUP/CH/RF/FEATURE/DYNPD/addr →
 * clear flags + FLUSH_RX → CONFIG=0x3F (PRX, powered) → CE-high. No power-down
 * cycle, so ~200 µs settle is enough. Called by the dongle radio watchdog.
 * (Mirrors the register block in rf_driver_init; keep them in sync.) */
void rf_driver_rearm_rx(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    if (!r->present) return;
    ce_low(r);                                       /* standby while reconfiguring */
    rf_driver_write_reg(r, REG_EN_AA, 0x01);
    rf_driver_write_reg(r, REG_EN_RXADDR, 0x01);
    rf_driver_write_reg(r, REG_SETUP_AW, 0x03);
    rf_driver_write_reg(r, REG_SETUP_RETR, 0x13);
    rf_driver_set_channel(r, cfg->channel);
    rf_driver_write_reg(r, REG_RF_SETUP, 0x0E);
    rf_driver_write_reg(r, REG_FEATURE, 0x04);
    rf_driver_write_reg(r, REG_DYNPD, 0x01);
    uint8_t addr[5];
    memcpy(addr, cfg->rx_addr, 4);
    addr[4] = cfg->addr_suffix;
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);
    rf_driver_write_reg(r, REG_STATUS, 0x70);        /* clear RX_DR|TX_DS|MAX_RT */
    { uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);        /* PRX, powered, RX_DR IRQ on */
    vTaskDelay(pdMS_TO_TICKS(2));                     /* Tpd2stby ≈1.5ms — MUST wait before
                                                        CE-high if the radio was powered down
                                                        (0x3C), else it never enters RX */
    ce_high(r);                                      /* resume listening */
}

/* Boot-time sanity check on a freshly-init'd PRX radio: read back the critical
 * registers and compare to the expected init values. Logs "verify OK" with the
 * read-back values, or "verify FAIL" with the exact diff per register.
 * Useful to spot a radio whose SPI writes silently didn't fully land — exactly
 * what we hit on the dongle's NRF1 (CONFIG ended up 0x3C instead of 0x3F due to
 * a marginal solder joint dropping bits 0,1 = PWR_UP/PRIM_RX). */
bool rf_driver_verify_rx(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    if (!r->present) {
        ESP_LOGW(TAG, "verify csn=%d skipped (not present)", cfg->pin_csn);
        return false;
    }
    uint8_t cfg_v = rf_driver_read_reg(r, REG_CONFIG);
    uint8_t en_aa = rf_driver_read_reg(r, REG_EN_AA);
    uint8_t en_rx = rf_driver_read_reg(r, REG_EN_RXADDR);
    uint8_t ch_v  = rf_driver_read_reg(r, REG_RF_CH);
    uint8_t rf_v  = rf_driver_read_reg(r, REG_RF_SETUP);

    /* RX_ADDR_P0 is a 5-byte register — read in one R_REGISTER transaction. */
    uint8_t tx[6] = { REG_RX_ADDR_P0, 0, 0, 0, 0, 0 };
    uint8_t rx[6] = {0};
    csn_low(r);
    spi_xfer(r, tx, rx, 6);
    csn_high(r);
    uint8_t addr_lsb = rx[1];   /* LSB = slot suffix in the KaSe addressing scheme */

    bool ok = (cfg_v == 0x3F)
           && (en_aa == 0x01)
           && (en_rx == 0x01)
           && (ch_v  == cfg->channel)
           && (rf_v  == 0x0E)
           && (addr_lsb == cfg->addr_suffix);

    if (ok) {
        ESP_LOGI(TAG, "verify csn=%d OK   CONFIG=0x%02x EN_AA=0x%02x EN_RX=0x%02x "
                      "RF_CH=%u RF_SETUP=0x%02x ADDR.lsb=0x%02x",
                 cfg->pin_csn, cfg_v, en_aa, en_rx, ch_v, rf_v, addr_lsb);
    } else {
        ESP_LOGW(TAG, "verify csn=%d FAIL CONFIG=0x%02x(exp 0x3F) EN_AA=0x%02x(exp 0x01) "
                      "EN_RX=0x%02x(exp 0x01) RF_CH=%u(exp %u) RF_SETUP=0x%02x(exp 0x0E) "
                      "ADDR.lsb=0x%02x(exp 0x%02x)",
                 cfg->pin_csn, cfg_v, en_aa, en_rx, ch_v, cfg->channel,
                 rf_v, addr_lsb, cfg->addr_suffix);
    }
    return ok;
}

/* ════════════════════════════════════════════════════════════════
 * PTX mode (half → dongle transmit)
 * Compiled only when KASE_HAS_RF_TX=y. Shares all helpers above.
 * ════════════════════════════════════════════════════════════════ */
#if CONFIG_KASE_HAS_RF_TX

/* Additional registers and commands for PTX mode */
#define REG_TX_ADDR         0x10
#define CMD_W_TX_PAYLOAD    0xA0
#define CMD_FLUSH_TX        0xE1

/* MAX_RT accumulator: still used for the FIFO-flush logic + debug. */
uint32_t rf_tx_max_rt_count = 0;

/* ARC_CNT accumulators: Σ per-packet retransmits + TX count since the last read.
 * read + reset by half_scan_task to compute PKT_HEARTBEAT.link_q (retry %). */
uint32_t rf_tx_retr_sum = 0;
uint32_t rf_tx_count    = 0;

esp_err_t rf_driver_init_tx(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;

    /* CSN + CE as GPIO outputs; CE low (PTX transmits only on CE pulse) */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << cfg->pin_csn) | (1ULL << cfg->pin_ce),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    csn_high(r);
    ce_low(r);

    /* SPI bus — half has one radio, always shares_bus_first=true */
    if (cfg->shares_bus_first) {
        spi_bus_config_t bus = {
            .mosi_io_num   = cfg->pin_mosi,
            .miso_io_num   = cfg->pin_miso,
            .sclk_io_num   = cfg->pin_sck,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            /* Half shares this bus with the SSD1681 e-ink, which writes the full
             * 5000-byte framebuffer (cmd 0x24) in one transaction. The NRF24 itself
             * only needs ~33 bytes, but the bus max must cover its largest device or
             * the e-ink RAM write fails silently (ESP_ERR_INVALID_ARG) → garbled panel. */
            .max_transfer_sz = 5120,
        };
        esp_err_t e = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = cfg->clock_hz,
        .mode           = 0,    /* CPOL=0, CPHA=0 — safe for GPIO45 strapping pin */
        .spics_io_num   = -1,   /* manual CSN */
        .queue_size     = 1,
        .command_bits   = 0, .address_bits = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(cfg->spi_host, &dev, &r->spi));

    vTaskDelay(pdMS_TO_TICKS(5));   /* power-on settle */

    if (!rf_driver_probe(r)) {
        r->present = false;
        return ESP_FAIL;
    }

    /* Configure registers in PTX mode */
    rf_driver_write_reg(r, REG_CONFIG,     0x00);   /* power down while configuring */
    rf_driver_write_reg(r, REG_EN_AA,      0x01);   /* auto-ack pipe 0 (dongle ACKs our TX) */
    rf_driver_write_reg(r, REG_EN_RXADDR,  0x01);   /* pipe 0 for ACK reception */
    rf_driver_write_reg(r, REG_SETUP_AW,   0x03);   /* 5-byte address */
    rf_driver_write_reg(r, REG_SETUP_RETR, 0x13);   /* ARD=500 µs (bits7:4=0x1), ARC=3 (bits3:0=0x3) */
    rf_driver_set_channel(r, cfg->channel);
    rf_driver_write_reg(r, REG_RF_SETUP,   0x0E);   /* 2 Mbps (RF_DR_HIGH=1, RF_DR_LOW=0), 0 dBm */
    rf_driver_write_reg(r, REG_FEATURE,    0x04);   /* EN_DPL (bit2) */
    rf_driver_write_reg(r, REG_DYNPD,      0x01);   /* DPL on pipe 0 */

    /* TX_ADDR and RX_ADDR_P0 must be the same 5-byte address for ESB auto-ACK.
     * Address = cfg->rx_addr[0..3] + cfg->addr_suffix. */
    uint8_t addr[5];
    memcpy(addr, cfg->rx_addr, 4);
    addr[4] = cfg->addr_suffix;
    write_reg_buf(r, REG_TX_ADDR,    addr, 5);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);   /* MUST match TX_ADDR for ESB ACK */

    /* Clear any stale IRQ flags, flush TX FIFO */
    rf_driver_write_reg(r, REG_STATUS, 0x70);   /* clear RX_DR|TX_DS|MAX_RT */
    {
        uint8_t c = CMD_FLUSH_TX, rx_byte;
        csn_low(r); spi_xfer(r, &c, &rx_byte, 1); csn_high(r);
    }

    /* PTX mode, power up.
     * CONFIG = 0x3E = 0b00111110:
     *   bit6 MASK_RX_DR=0 (RX IRQ enabled, though no RX in PTX)
     *   bit5 MASK_TX_DS=1 (TX_DS IRQ masked — we poll STATUS)
     *   bit4 MASK_MAX_RT=1 (MAX_RT IRQ masked — we poll STATUS)
     *   bit3 EN_CRC=1
     *   bit2 CRCO=1 (2-byte CRC)
     *   bit1 PWR_UP=1
     *   bit0 PRIM_RX=0 (PTX mode)
     * MVP uses polling (STATUS register), so IRQ masks don't affect functionality.
     * Masking TX_DS and MAX_RT keeps the IRQ pin clean in case it's monitored later. */
    rf_driver_write_reg(r, REG_CONFIG, 0x3E);
    vTaskDelay(pdMS_TO_TICKS(2));   /* Tpd2stby = 1.5 ms (datasheet) */
    /* CE stays LOW in PTX; it is pulsed only during rf_driver_send() */

    r->present = true;
    ESP_LOGI(TAG, "radio PTX ch=0x%02x addr=KaSe.%02x init OK", cfg->channel, cfg->addr_suffix);
    return ESP_OK;
}

bool rf_driver_send(rf_radio_t *r, const uint8_t *buf, uint8_t len)
{
    /* Write payload to TX FIFO */
    uint8_t tx[33], rx_buf[33];
    tx[0] = CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], buf, len);
    csn_low(r);
    spi_xfer(r, tx, rx_buf, (size_t)(len + 1));
    csn_high(r);

    /* Pulse CE high ≥ 10 µs to trigger transmission */
    ce_high(r);
    esp_rom_delay_us(15);   /* 15 µs > 10 µs minimum (datasheet Table 15) */
    ce_low(r);

    /* Poll STATUS until TX_DS (bit5 = ACK received) or MAX_RT (bit4 = all retries failed).
     * Worst-case time: ARC=3 retries × ARD=500 µs × 2 (on-air) ≈ 3 ms + margin → 5 ms. */
    uint32_t deadline_us = (uint32_t)(esp_timer_get_time() + 5000);
    uint8_t status;
    do {
        status = rf_driver_read_reg(r, REG_STATUS);
        if (status & 0x30) break;   /* TX_DS=bit5 or MAX_RT=bit4 set */
    } while ((uint32_t)esp_timer_get_time() < deadline_us);

    bool success = (status & 0x20) != 0;   /* bit5 TX_DS = ACK received */

    if (status & 0x10) {
        /* MAX_RT: flush TX FIFO or the failed packet blocks all future sends */
        uint8_t c = CMD_FLUSH_TX;
        csn_low(r); spi_xfer(r, &c, rx_buf, 1); csn_high(r);
        rf_tx_max_rt_count++;
    }

    /* Accumulate this packet's retransmit count for the link-quality metric.
     * OBSERVE_TX ARC_CNT (bits 3:0) resets at the next TX; on MAX_RT it reads the
     * max (ARC=3). Must be read before the next send. */
    rf_tx_retr_sum += (uint32_t)(rf_driver_read_reg(r, REG_OBSERVE_TX) & 0x0F);
    rf_tx_count++;

    /* Clear TX_DS + MAX_RT flags in STATUS (write 1 to clear) */
    rf_driver_write_reg(r, REG_STATUS, 0x30);

    if (success) r->pkt_rx++;   /* reuse pkt_rx as pkt_tx_ok counter */
    return success;
}

void rf_driver_set_tx_address(rf_radio_t *r, const uint8_t addr[5])
{
    ce_low(r);
    write_reg_buf(r, REG_TX_ADDR,    addr, 5);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);   /* must match for ESB ACK */
}

uint16_t rf_driver_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t addr[5],
                               uint8_t *buf, uint16_t maxlen, uint32_t timeout_ms)
{
    ce_low(r);
    rf_driver_set_channel(r, ch);
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);
    rf_driver_write_reg(r, REG_CONFIG, 0x3F);    /* PRX power-up */
    vTaskDelay(pdMS_TO_TICKS(2));
    rf_driver_write_reg(r, REG_STATUS, 0x70);
    { uint8_t c = CMD_FLUSH_RX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }
    ce_high(r);

    uint16_t got = 0;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + timeout_ms;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        if (rf_driver_rx_available(r)) {
            got = rf_driver_read_rx(r, buf, maxlen);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ce_low(r);
    /* Restore PTX mode: rf_driver_send() does NOT write CONFIG (it assumes PTX),
     * so without this the radio stays in PRX (0x3F) and every subsequent REQ in
     * the pairing loop is silently NOT transmitted — only REQ #1 ever goes out.
     * (Root cause of "one half pairs, the other never does": whoever's REQ #1
     * happened to be ACKed paired; the other never re-sent.) */
    rf_driver_write_reg(r, REG_CONFIG, 0x3E);   /* PTX power-up, EN_CRC|CRCO */
    return got;
}

#endif /* CONFIG_KASE_HAS_RF_TX */

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
