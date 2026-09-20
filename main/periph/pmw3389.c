/* See pmw3389.h. Ported from the bring-up validated on the board on
 * 2026-08-25 (~/Documents/GitHub/Conchodytes/bringup/), itself derived from
 * the mornepousse/Mase POC and from mrjohnk/PMW3389DM — with three
 * corrections that neither one carries: timings compliant with the 3389,
 * startup order, and the right SROM blob. */

#include "pmw3389.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#include "board.h"

static const char *TAG = "pmw3389";

/* -- Registers, Table of §5.1, p. 20 ─────────────────────────────────────── */
#define REG_PRODUCT_ID          0x00
#define REG_REVISION_ID         0x01
#define REG_MOTION              0x02
#define REG_DELTA_X_L           0x03
#define REG_DELTA_X_H           0x04
#define REG_DELTA_Y_L           0x05
#define REG_DELTA_Y_H           0x06
#define REG_SQUAL               0x07
#define REG_SHUTTER_LOWER       0x0B
#define REG_SHUTTER_UPPER       0x0C
#define REG_RESOLUTION_L        0x0E   /* 16 bits on the 3389, not a Config1 */
#define REG_RESOLUTION_H        0x0F
#define REG_CONFIG2             0x10
#define REG_SROM_ENABLE         0x13
#define REG_SROM_ID             0x2A
#define REG_POWER_UP_RESET      0x3A
#define REG_INVERSE_PRODUCT_ID  0x3F
#define REG_MOTION_BURST        0x50
#define REG_SROM_LOAD_BURST     0x62

/* Motion_Burst layout — ESTABLISHED BY MEASUREMENT on 2026-08-25, for lack of
 * having it in a datasheet. Neither the 3389's 20 pages nor the 3360's
 * describe it; they only give its timings (tSRAD_MOTBR 35 us, tBEXIT 500 ns,
 * Table 5, p. 16).
 *
 * Method: display the burst bytes next to the same registers read one at a
 * time, then sweep on a single axis at a time, mouse lifted on the return
 * so the sign does not cancel out.
 *
 * Evidence retained:
 *   [0]  always equals the reference + 0x80 — the MOT bit, armed by the burst
 *        and cleared by the next read. Seen three times.
 *   [1]  constant at 0x7F.
 *   [2,3] 100.0% bias over 76 samples of a horizontal sweep.
 *   [4,5] 99.8% bias over 71 samples of a vertical sweep.
 *   [6]  same range as SQUAL read separately.
 *   [8]  peaks at 0x7F — a pixel is worth at most 127.
 *   [10] zero, like Shutter_Upper; [11] same range as Shutter_Lower.
 *   [12..15] always zero: the burst is 12 bytes, no more. */
#define BURST_LEN            12
#define BURST_MOTION          0
#define BURST_OBSERVATION     1
#define BURST_DELTA_X_L       2
#define BURST_DELTA_X_H       3
#define BURST_DELTA_Y_L       4
#define BURST_DELTA_Y_H       5
#define BURST_SQUAL           6
#define BURST_RAWDATA_SUM     7
#define BURST_MAX_RAWDATA     8
#define BURST_MIN_RAWDATA     9
#define BURST_SHUTTER_UPPER  10
#define BURST_SHUTTER_LOWER  11

/* -- Timings, Table 5, p. 16 -------------------------------------------------
 * The 3389's values, NOT the 3360's. Deliberate margin above the
 * minimum: these waits are not in the critical path, and bench experience
 * is that a too-short tSRAD passes then fails intermittently. */
#define T_SRAD_US   180   /* min 160 */
#define T_SRW_US     20   /* min 20  */
#define T_SWW_US    200   /* min 180 */
#define T_SROM_US    15   /* between SROM burst bytes */

extern const unsigned short pmw3389_firmware_length;
extern const unsigned char pmw3389_firmware_data[];

static spi_device_handle_t s_dev;
static bool s_ready;

/* -- Transport ----------------------------------------------------------------
 * CS driven by hand, and this is not a style choice: `tSRAD` falls BETWEEN
 * the address byte and the data byte. ESP-IDF's hardware CS would
 * raise it in the middle of the wait, which aborts the read. Hence
 * `spics_io_num = -1` — the same move as comm/rf/rf_driver.c for the nRF24. */

static inline void cs_low(void)
{
    gpio_set_level(BOARD_SNS_NCS_GPIO, 0);
    esp_rom_delay_us(1);
}

static inline void cs_high(void)
{
    esp_rom_delay_us(1);
    gpio_set_level(BOARD_SNS_NCS_GPIO, 1);
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

static uint8_t reg_read(uint8_t addr)
{
    uint8_t a = addr & 0x7F;      /* MSB at 0 = read */
    uint8_t d = 0;

    cs_low();
    xfer(&a, NULL, 1);
    esp_rom_delay_us(T_SRAD_US);
    xfer(NULL, &d, 1);
    cs_high();
    esp_rom_delay_us(T_SRW_US);
    return d;
}

static void reg_write(uint8_t addr, uint8_t val)
{
    uint8_t buf[2] = { (uint8_t)(addr | 0x80), val };   /* MSB at 1 = write */

    cs_low();
    xfer(buf, NULL, 2);
    esp_rom_delay_us(T_SRW_US);
    cs_high();
    esp_rom_delay_us(T_SWW_US);
}

/* -- SROM upload --------------------------------------------------------------
 * 4094 bytes at 15 us per byte: ~61 ms during which NOTHING else must
 * touch the bus. `spi_device_acquire_bus` guarantees this against the
 * nRF24 — a radio frame in the middle would corrupt the SROM, and the
 * sensor would start on invalid firmware without necessarily saying so. */
static esp_err_t srom_upload(void)
{
    reg_write(REG_CONFIG2, 0x20);        /* register's default value, p. 20 */
    reg_write(REG_SROM_ENABLE, 0x1D);
    vTaskDelay(pdMS_TO_TICKS(10));       /* more than one frame period */
    reg_write(REG_SROM_ENABLE, 0x18);

    uint8_t burst = REG_SROM_LOAD_BURST | 0x80;
    cs_low();
    xfer(&burst, NULL, 1);
    esp_rom_delay_us(T_SROM_US);
    for (unsigned i = 0; i < pmw3389_firmware_length; i++) {
        xfer(&pmw3389_firmware_data[i], NULL, 1);
        esp_rom_delay_us(T_SROM_US);
    }
    cs_high();
    esp_rom_delay_us(200);

    uint8_t srom_id = reg_read(REG_SROM_ID);
    ESP_LOGI(TAG, "SROM_ID = 0x%02X", srom_id);
    if (srom_id == 0x00) {
        ESP_LOGE(TAG, "SROM upload failed: the sensor will not track anything");
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 0x00 = Rest disabled. Power management will come with the radio; in
     * the meantime, a sensor that sleeps on its own skews every bench measurement. */
    reg_write(REG_CONFIG2, 0x00);
    return ESP_OK;
}

/* Resetting the serial port THEN a software reset — in this order, and
 * before the first register read. See the pmw3389_init comment in the
 * header: without this the first read comes out shifted by two bits. */
static void sensor_reset(void)
{
    cs_high(); cs_low(); cs_high();
    reg_write(REG_POWER_UP_RESET, 0x5A);
    vTaskDelay(pdMS_TO_TICKS(50));       /* tMOT-RST = 50 ms, Table 5, p. 16 */

    (void)reg_read(REG_MOTION);
    (void)reg_read(REG_DELTA_X_L);
    (void)reg_read(REG_DELTA_X_H);
    (void)reg_read(REG_DELTA_Y_L);
    (void)reg_read(REG_DELTA_Y_H);
}

esp_err_t pmw3389_probe(uint8_t *id, uint8_t *inverse, uint8_t *revision)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t i = reg_read(REG_PRODUCT_ID);
    uint8_t v = reg_read(REG_INVERSE_PRODUCT_ID);
    uint8_t r = reg_read(REG_REVISION_ID);
    if (id) *id = i;
    if (inverse) *inverse = v;
    if (revision) *revision = r;
    return ESP_OK;
}

/* Sets the resolution in cpi.
 *
 * WARNING: THE FORMULA IS NOT IN THE DATASHEET available. The library
 * version (PMW3389DM-T3QU v1.0, 07 sep 2017, 20 pages) gives the summary
 * table p. 20 — `0x0E Resolution_L` RW default 0x00, `0x0F Resolution_H` RW
 * default 0x42 — and "up to 16000 cpi" p. 1, but NO bit-by-bit description
 * of the two registers. The encoding used here, a 50 cpi step over 16 bits
 * little-endian, is the one from public-domain PMW3389 drivers. It is
 * therefore to be VALIDATED IN USE, and `pmw3389_init()` logs the value
 * read back from the chip so it can be checked against reality rather than believed.
 *
 * 16000 cpi max => 320 steps, which fit on 9 bits: hence the L/H pair. */
void pmw3389_set_cpi(uint16_t cpi)
{
    if (cpi < 50)    cpi = 50;
    if (cpi > 16000) cpi = 16000;
    uint16_t pas = (uint16_t)(cpi / 50u);
    reg_write(REG_RESOLUTION_L, (uint8_t)(pas & 0xFF));
    reg_write(REG_RESOLUTION_H, (uint8_t)(pas >> 8));
}

esp_err_t pmw3389_init(void)
{
    /* Select lines BEFORE everything else. The nRF24 shares the bus: its
     * CSN must be high and its CE low before a single byte circulates,
     * otherwise two slaves drive MISO at the same time. */
    gpio_config_t sel = {
        .pin_bit_mask = (1ULL << BOARD_SNS_NCS_GPIO) |
                        (1ULL << BOARD_NRF_CSN) | (1ULL << BOARD_NRF_CE),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sel));
    gpio_set_level(BOARD_SNS_NCS_GPIO, 1);
    gpio_set_level(BOARD_NRF_CSN, 1);
    gpio_set_level(BOARD_NRF_CE, 0);

    gpio_config_t motion = {
        .pin_bit_mask = 1ULL << BOARD_SNS_MOTION_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&motion));

    /* The bus is shared. When the radio relay is compiled for this role, it
     * may have initialized it before us: ESP_ERR_INVALID_STATE then means
     * "already done", and that's fine. Any other error is real. */
    spi_bus_config_t bus = {
        .sclk_io_num     = BOARD_NRF_SCK,
        .mosi_io_num     = BOARD_NRF_MOSI,
        .miso_io_num     = BOARD_NRF_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(BOARD_NRF_SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_SNS_CLOCK_HZ,
        .mode           = BOARD_SNS_SPI_MODE,   /* 3 — the nRF24 is in 0 */
        .spics_io_num   = -1,                   /* manual CS, see above */
        .queue_size     = 1,
    };
    err = spi_bus_add_device(BOARD_NRF_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(50));
    sensor_reset();

    uint8_t id = 0, inv = 0, rev = 0;
    pmw3389_probe(&id, &inv, &rev);
    ESP_LOGI(TAG, "Product_ID=0x%02X Inverse=0x%02X Revision=0x%02X", id, inv, rev);

    if ((uint8_t)(id ^ inv) != 0xFF) {
        /* The complement does not hold: this is not an unexpected chip, it
         * is the bus lying. A cut line, a stuck line, a wrong SPI mode, or
         * the nRF24 answering in place of the sensor. */
        ESP_LOGE(TAG, "0x%02X ^ 0x%02X != 0xFF: the bus is lying, not the chip", id, inv);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (id != PMW3389_PRODUCT_ID) {
        ESP_LOGE(TAG, "0x%02X is not a PMW3389 (expected 0x%02X)",
                 id, PMW3389_PRODUCT_ID);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_ERROR_CHECK(spi_device_acquire_bus(s_dev, portMAX_DELAY));
    err = srom_upload();
    spi_device_release_bus(s_dev);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(10));
    s_ready = true;

    /* The firmware did NOT set the resolution: the chip stayed on whatever
     * the reset and the SROM leave it at, hence a cursor way too fast.
     * The value in place is read BEFORE setting it — the datasheet's
     * register table gives the reset defaults, not what the SROM leaves. */
    uint8_t rl = reg_read(REG_RESOLUTION_L), rh = reg_read(REG_RESOLUTION_H);
    ESP_LOGI(TAG, "resolution found: L=0x%02X H=0x%02X", rl, rh);

    pmw3389_set_cpi(BOARD_SNS_CPI);
    rl = reg_read(REG_RESOLUTION_L); rh = reg_read(REG_RESOLUTION_H);
    ESP_LOGI(TAG, "resolution set to %d cpi: L=0x%02X H=0x%02X",
             BOARD_SNS_CPI, rl, rh);
    ESP_LOGI(TAG, "mounting: BOARD_SNS_ROT_180=%d%s", BOARD_SNS_ROT_180,
             BOARD_SNS_ROT_180 ? " (dx and dy negated)" : " (raw axes)");
    return ESP_OK;
}

esp_err_t pmw3389_read_motion(pmw3389_motion_t *out)
{
    if (!s_ready || !out) return ESP_ERR_INVALID_STATE;

    /* ONE transaction instead of nine.
     *
     * The old version did one write then eight register reads:
     * 230 us + 8 x 210 us ~= 1.9 ms, most of it in tSRAD (160 us per
     * read). The burst only pays for one, and at 35 us: ~90 us total,
     * a factor of 21. On a mouse this is not just comfort — at 1.9 ms the
     * reading blocked click polling a quarter of the time, and capped
     * the report rate at 500 Hz.
     *
     * It also returns a COHERENT snapshot as a bonus. The separate reads
     * sampled different frames: SQUAL read this way dropped to
     * 15-40 when the same register read alone was worth 80, which had at
     * one point wrongly cast suspicion on the optics. */
    uint8_t b[BURST_LEN];

    /* WARNING: THE BUS IS SHARED WITH THE nRF24, AND BOTH DRIVE THEIR CS BY
     * HAND (`spics_io_num = -1`). ESP-IDF therefore does not know where a
     * logical transaction starts or ends: between the address byte and the
     * data, this driver keeps CS low for 35 us without sending anything,
     * and the driver thinks it's free. So it is serialized explicitly.
     *
     * WARNING: this does NOT solve the coexistence of two different SPI
     * MODES (3 here, 0 for the radio): the mode switch happens at the start
     * of the transaction, so after the caller has already lowered its CS.
     * That is what killed the radio link on 2026-08-26, and it is fixed on
     * the radio side by `spi_parquer_mode()` in comm/rf/rf_driver.c — not here. */
    spi_device_acquire_bus(s_dev, portMAX_DELAY);

    reg_write(REG_MOTION_BURST, 0x00);      /* arms burst mode */

    uint8_t addr = REG_MOTION_BURST;        /* MSB at 0: read */
    cs_low();
    xfer(&addr, NULL, 1);
    esp_rom_delay_us(35);                   /* tSRAD_MOTBR, Table 5, p. 16 */
    xfer(NULL, b, BURST_LEN);               /* the 12 bytes back to back */
    cs_high();
    esp_rom_delay_us(5);                    /* tBEXIT = 500 ns, generous */

    /* WARNING: THE MOT BIT RULES: without movement since the last read, the
     * Delta registers do NOT carry a zero displacement, they carry whatever.
     * Reading them without checking this bit amounts to injecting noise
     * into the cursor — that is what made the "mouse move on its own" on
     * the bench on 2026-08-26. The byte was already fetched by the burst
     * (offset 0), it simply was not being checked. */
    out->motion  = (b[BURST_MOTION] & 0x80) != 0;
    if (out->motion) {
        out->dx = (int16_t)(((uint16_t)b[BURST_DELTA_X_H] << 8) | b[BURST_DELTA_X_L]);
        out->dy = (int16_t)(((uint16_t)b[BURST_DELTA_Y_H] << 8) | b[BURST_DELTA_Y_L]);
#if BOARD_SNS_ROT_180
        /* Sensor mounted at 180 degrees: both axes are flipped, they are put
         * back upright here. See BOARD_SNS_ROT_180 in board.h — the constant
         * goes to 0 the day the layout rotates the footprint. */
        out->dx = (int16_t)(-out->dx);
        out->dy = (int16_t)(-out->dy);
#endif
    } else {
        out->dx = 0;
        out->dy = 0;
    }
    out->squal   = b[BURST_SQUAL];
    spi_device_release_bus(s_dev);

    out->shutter = (uint16_t)(((uint16_t)b[BURST_SHUTTER_UPPER] << 8) |
                               b[BURST_SHUTTER_LOWER]);

    return ESP_OK;
}

bool pmw3389_motion_pending(void)
{
    return gpio_get_level(BOARD_SNS_MOTION_GPIO) == 0;   /* active low */
}
