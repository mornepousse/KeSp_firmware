#include "nrf24_receiver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "keyboard_manager.h"
#include "status_display.h"
#include <string.h>

#define TAG "NRF24"

// Pinout
#define PIN_MOSI 5
#define PIN_MISO 4
#define PIN_SCK  6
#define PIN_CSN  48
#define PIN_CE   47
#define PIN_PWR  36
#define PIN_IRQ  45

// NRF24 Commands
#define R_REGISTER    0x00
#define W_REGISTER    0x20
#define R_RX_PAYLOAD  0x61
#define W_TX_PAYLOAD  0xA0
#define FLUSH_TX      0xE1
#define FLUSH_RX      0xE2
#define NOP           0xFF

// Registers
#define CONFIG      0x00
#define EN_AA       0x01
#define EN_RXADDR   0x02
#define SETUP_AW    0x03
#define SETUP_RETR  0x04
#define RF_CH       0x05
#define RF_SETUP    0x06
#define STATUS      0x07
#define RX_ADDR_P0  0x0A
#define RX_PW_P0    0x11
#define FIFO_STATUS 0x17
#define DYNPD       0x1C
#define FEATURE     0x1D
#define RPD         0x09

static spi_device_handle_t spi;

/* Task handle to notify nrf task from IRQ ISR */
TaskHandle_t nrf24_task_handle = NULL;

/* Diagnostics: enable verbose NRF24 debug logging (default OFF to reduce log noise) */
#ifndef NRF24_DEBUG_VERBOSE
//#define NRF24_DEBUG_VERBOSE 0
#endif

/* Control how often to log payloads when verbose is enabled */
#ifndef NRF24_PAYLOAD_LOG_EVERY
#define NRF24_PAYLOAD_LOG_EVERY 25
#endif

/* Watchdog / recovery configuration */
#ifndef NRF24_RECOVER_TIMEOUT_MS
#define NRF24_RECOVER_TIMEOUT_MS 1500 /* more aggressive: 1.5s */
#endif
#ifndef NRF24_RECOVER_COOLDOWN_MS
#define NRF24_RECOVER_COOLDOWN_MS 10000
#endif

/* Static address used for reconfiguration on recover */
static const uint8_t NRF24_ADDR[5] = {0xCE, 0xCE, 0xCE, 0xCE, 0xCE};

/* Throttle periodic status logs to reduce noise */
#ifndef NRF24_STATUS_LOG_INTERVAL_MS
#define NRF24_STATUS_LOG_INTERVAL_MS 20000
#endif

/* ISR: notify the nrf24 task to process FIFO */
static volatile uint32_t nrf_isr_count = 0;    // increments in ISR
static volatile uint32_t nrf_notify_count = 0; // increments in task when processing notification
static volatile TickType_t nrf_last_packet_tick = 0; // last time we processed a valid payload
static uint32_t nrf_recover_count = 0; // number of recovery attempts
static TickType_t nrf_last_recover_tick = 0;
static uint32_t nrf_payload_seen = 0; // total payloads seen (for throttled logging)

static void IRAM_ATTR nrf_irq_isr(void* arg)
{
    BaseType_t woke = pdFALSE;
#ifdef NRF24_DEBUG_VERBOSE
    nrf_isr_count++;
#endif
    if (nrf24_task_handle != NULL) {
        vTaskNotifyGiveFromISR(nrf24_task_handle, &woke);
        portYIELD_FROM_ISR(woke);
    }
}

static void nrf_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx_data[2] = {W_REGISTER | (reg & 0x1F), value};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_data,
    };
    spi_device_polling_transmit(spi, &t);
}

static void nrf_write_buf(uint8_t reg, const uint8_t *data, uint8_t len) {
    uint8_t *tx_buf = heap_caps_malloc(len + 1, MALLOC_CAP_DMA);
    if (!tx_buf) return;

    tx_buf[0] = W_REGISTER | (reg & 0x1F);
    memcpy(&tx_buf[1], data, len);

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buf,
    };
    spi_device_polling_transmit(spi, &t);
    free(tx_buf);
}

static void nrf_read_buf(uint8_t reg, uint8_t *data, uint8_t len) {
    uint8_t *tx_buf = heap_caps_malloc(len + 1, MALLOC_CAP_DMA);
    uint8_t *rx_buf = heap_caps_malloc(len + 1, MALLOC_CAP_DMA);
    if (!tx_buf || !rx_buf) {
        free(tx_buf);
        free(rx_buf);
        return;
    }

    memset(tx_buf, 0xFF, len + 1);
    tx_buf[0] = R_REGISTER | (reg & 0x1F);

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    spi_device_polling_transmit(spi, &t);
    
    memcpy(data, &rx_buf[1], len);
    
    free(tx_buf);
    free(rx_buf);
}

static uint8_t nrf_read_reg(uint8_t reg) {
    uint8_t tx[2] = {R_REGISTER | (reg & 0x1F), 0xFF};
    uint8_t rx[2] = {0};
    
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    
    spi_device_polling_transmit(spi, &t);
    return rx[1];
}

static void nrf_read_payload(uint8_t *data, uint8_t len) {
    uint8_t *tx_buf = heap_caps_malloc(len + 1, MALLOC_CAP_DMA);
    uint8_t *rx_buf = heap_caps_malloc(len + 1, MALLOC_CAP_DMA);
    if (!tx_buf || !rx_buf) {
        free(tx_buf);
        free(rx_buf);
        return;
    }

    memset(tx_buf, 0, len + 1);
    tx_buf[0] = R_RX_PAYLOAD;

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    spi_device_polling_transmit(spi, &t);

    memcpy(data, &rx_buf[1], len);

    free(tx_buf);
    free(rx_buf);
}

static void nrf_flush_rx() {
    uint8_t cmd = FLUSH_RX;
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .flags = SPI_TRANS_USE_TXDATA,
    };
    spi_device_polling_transmit(spi, &t);
}

static uint8_t nrf_get_status() {
    uint8_t cmd = NOP;
    uint8_t status = 0;
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .rx_buffer = &status,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
    };
    spi_device_polling_transmit(spi, &t);
    return status;
}

void nrf24_init(void) {
    // Power ON
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_PWR) | (1ULL << PIN_CE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // IRQ Pin Config (enable falling edge IRQ)
    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&irq_conf);

    // Install ISR service and attach handler
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GPIO ISR service already installed");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %d", ret);
    }
    gpio_isr_handler_add(PIN_IRQ, nrf_irq_isr, NULL);

    gpio_set_level(PIN_PWR, 0); // Low to turn ON P-channel (Active Low)
    gpio_set_level(PIN_CE, 0);

    vTaskDelay(pdMS_TO_TICKS(100)); // Wait for power up

    // SPI Init
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_MISO,
        .mosi_io_num = PIN_MOSI,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    
    // Use SPI2_HOST
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Init Failed: %d", ret);
        return;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4000000, // 4 MHz
        .mode = 0,
        .spics_io_num = PIN_CSN,
        .queue_size = 7,
    };
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Device Add Failed: %d", ret);
        return;
    }

    // NRF Config
    nrf_write_reg(CONFIG, 0x00); // Power Down first
    nrf_write_reg(EN_AA, 0x00);  // No Auto Ack
    nrf_write_reg(EN_RXADDR, 0x01); // Enable Pipe 0
    nrf_write_reg(SETUP_AW, 0x03); // 5 bytes address
    nrf_write_reg(SETUP_RETR, 0x00); // Retransmit disabled
    nrf_write_reg(RF_CH, 76); // Channel 76
    nrf_write_reg(RF_SETUP, 0x06); // 1Mbps, 0dBm
    
    uint8_t addr[5] = {0xCE, 0xCE, 0xCE, 0xCE, 0xCE};
    nrf_write_buf(RX_ADDR_P0, addr, 5);
    
    nrf_write_reg(RX_PW_P0, 5); // 5 bytes payload
    
    nrf_write_reg(DYNPD, 0x00); // No dynamic payload
    nrf_write_reg(FEATURE, 0x00); // No features

    // Verify configuration
    uint8_t check_ch = nrf_read_reg(RF_CH);
    uint8_t check_setup = nrf_read_reg(RF_SETUP);
    uint8_t check_addr[5];
    nrf_read_buf(RX_ADDR_P0, check_addr, 5);

    ESP_LOGI(TAG, "NRF Check: CH=%d (Exp 76), SETUP=0x%02X (Exp 0x06), ADDR=%02X:%02X:%02X:%02X:%02X", 
             check_ch, check_setup, 
             check_addr[0], check_addr[1], check_addr[2], check_addr[3], check_addr[4]);

    nrf_flush_rx();
    
    // Power Up and RX Mode
    // CONFIG: PRIM_RX=1, PWR_UP=1, CRC=1 (16-bit)
    // EN_CRC (bit 3) = 1
    // CRCO (bit 2) = 1 (2 bytes)
    // PWR_UP (bit 1) = 1
    // PRIM_RX (bit 0) = 1
    // Total: 0000 1111 = 0x0F
    nrf_write_reg(CONFIG, 0x0F);
    
    // CE High to start listening
    gpio_set_level(PIN_CE, 1);
    
    ESP_LOGI(TAG, "NRF24 Initialized (Fixed Config: CH76, 1Mbps, CRC16)");
}

bool nrf24_check_spi(void) {
    uint8_t check_ch = nrf_read_reg(RF_CH);
    return (check_ch == 76);
}

void nrf24_task(void *arg) {
    nrf24_init();

    uint8_t payload[32];
    uint32_t packet_count = 0;
    TickType_t last_log_time = xTaskGetTickCount();

    // Force Channel 76
    nrf_write_reg(RF_CH, 76);
    ESP_LOGI(TAG, "NRF24 Receiver ready on CH 76 (IRQ-driven)");

    /* Save our task handle so ISR can notify us */
    nrf24_task_handle = xTaskGetCurrentTaskHandle();

    while (1) {
        /* Quick check: if IRQ pin is asserted (active-low), drain FIFO immediately without waiting */
        if (gpio_get_level(PIN_IRQ) == 0) {
            uint8_t fifo = nrf_read_reg(FIFO_STATUS);
            while (!(fifo & 0x01)) {
                nrf_read_payload(payload, 5);
                nrf_write_reg(STATUS, 0x70);

                nrf_payload_seen++;
#ifdef NRF24_DEBUG_VERBOSE
                if ((nrf_payload_seen % NRF24_PAYLOAD_LOG_EVERY) == 0) {
                    ESP_LOGI(TAG, "NRF RX payload raw (sampled, immediate): %02X %02X %02X %02X %02X",
                             payload[0], payload[1], payload[2], payload[3], payload[4]);
                }
#endif

                if (payload[0] == 0x01) {
                    nrf_last_packet_tick = xTaskGetTickCount();
                    send_mouse_report(payload[1], (int8_t)payload[2], (int8_t)payload[3], (int8_t)payload[4]);
                    packet_count++;
                }

                fifo = nrf_read_reg(FIFO_STATUS);
                taskYIELD();
            }

            /* After immediate drain, continue to next loop iteration */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* Wait for IRQ notification (short timeout so we can detect missed IRQ quickly) */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (notified) {
#ifdef NRF24_DEBUG_VERBOSE
            nrf_notify_count++;
            uint8_t status_reg = nrf_get_status();
            uint8_t fifo = nrf_read_reg(FIFO_STATUS);
            ESP_LOGI(TAG, "NRF IRQ: isr_count=%lu notify_count=%lu status=0x%02X fifo=0x%02X",
                     (unsigned long)nrf_isr_count, (unsigned long)nrf_notify_count, status_reg, fifo);
#else
            uint8_t fifo = nrf_read_reg(FIFO_STATUS);
#endif

            /* Process all packets in FIFO */
            while (!(fifo & 0x01)) {
                nrf_read_payload(payload, 5);
                nrf_write_reg(STATUS, 0x70); // Clear all IRQ flags

                nrf_payload_seen++;
#ifdef NRF24_DEBUG_VERBOSE
                if ((nrf_payload_seen % NRF24_PAYLOAD_LOG_EVERY) == 0) {
                    ESP_LOGI(TAG, "NRF RX payload raw (sampled): %02X %02X %02X %02X %02X",
                             payload[0], payload[1], payload[2], payload[3], payload[4]);
                }
#endif

                if (payload[0] == 0x01) {
                    /* mark last packet tick for watchdog */
                    nrf_last_packet_tick = xTaskGetTickCount();
                    send_mouse_report(payload[1], (int8_t)payload[2], (int8_t)payload[3], (int8_t)payload[4]);
                    packet_count++;
                }

                fifo = nrf_read_reg(FIFO_STATUS);

                /* Yield occasionally when processing bursts to allow other tasks/idle to run */
                taskYIELD();
            }
        }

        /* Periodic log */
        if ((xTaskGetTickCount() - last_log_time) >= pdMS_TO_TICKS(NRF24_STATUS_LOG_INTERVAL_MS)) {
#ifdef NRF24_DEBUG_VERBOSE
            uint8_t status_reg = nrf_get_status();
            uint8_t fifo = nrf_read_reg(FIFO_STATUS);
            uint8_t rpd = nrf_read_reg(RPD);
            ESP_LOGI(TAG, "NRF24 | Packets: %lu isr_count=%lu notify_count=%lu status=0x%02X fifo=0x%02X RPD=%d last_pkt_age_ms=%lu recover_count=%lu",
                     packet_count, (unsigned long)nrf_isr_count, (unsigned long)nrf_notify_count, status_reg, fifo, rpd,
                     (unsigned long)((xTaskGetTickCount() - nrf_last_packet_tick) * portTICK_PERIOD_MS), (unsigned long)nrf_recover_count);
#else
            ESP_LOGI(TAG, "NRF24 | Packets: %lu last_pkt_age_ms=%lu recover_count=%lu",
                     packet_count, (unsigned long)((xTaskGetTickCount() - nrf_last_packet_tick) * portTICK_PERIOD_MS), (unsigned long)nrf_recover_count);
#endif
            packet_count = 0;
            last_log_time = xTaskGetTickCount();

            /* Watchdog: if no packet recently, attempt recovery */
            if (nrf_last_packet_tick != 0 && (xTaskGetTickCount() - nrf_last_packet_tick) > pdMS_TO_TICKS(NRF24_RECOVER_TIMEOUT_MS)) {
                /* avoid hammering recovery; apply cooldown */
                if ((xTaskGetTickCount() - nrf_last_recover_tick) > pdMS_TO_TICKS(NRF24_RECOVER_COOLDOWN_MS)) {
                    nrf_recover_count++;
                    nrf_last_recover_tick = xTaskGetTickCount();
                    ESP_LOGW(TAG, "NRF24: No packets for %lu ms - attempting recovery #%lu",
                             (unsigned long)((xTaskGetTickCount() - nrf_last_packet_tick) * portTICK_PERIOD_MS), (unsigned long)nrf_recover_count);

                    /* quick recovery: toggle CE and reflush registers and re-install ISR */
                    int irq_level_before = gpio_get_level(PIN_IRQ);
                    ESP_LOGI(TAG, "NRF24: PIN_IRQ level before recovery = %d", irq_level_before);

                    gpio_set_level(PIN_CE, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    nrf_flush_rx();
                    nrf_write_reg(STATUS, 0x70);
                    nrf_write_reg(RF_CH, 76);

                    /* Reinstall ISR handler as an extra mitigation */
                    gpio_isr_handler_remove(PIN_IRQ);
                    gpio_isr_handler_add(PIN_IRQ, nrf_irq_isr, NULL);

                    /* Aggressive mitigation every 3 recoveries: power-cycle module */
                    if ((nrf_recover_count % 3) == 0) {
                        ESP_LOGW(TAG, "NRF24: performing POWER-CYCLE as aggressive mitigation (recover #%lu)", (unsigned long)nrf_recover_count);
                        gpio_set_level(PIN_CE, 0);
                        /* power off (active-low P-channel) */
                        gpio_set_level(PIN_PWR, 1);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        /* power on */
                        gpio_set_level(PIN_PWR, 0);
                        vTaskDelay(pdMS_TO_TICKS(200));

                        /* reapply essential registers */
                        nrf_flush_rx();
                        nrf_write_reg(STATUS, 0x70);
                        nrf_write_reg(RF_CH, 76);
                        nrf_write_reg(RF_SETUP, 0x06);
                        nrf_write_reg(RX_PW_P0, 5);
                        nrf_write_buf(RX_ADDR_P0, NRF24_ADDR, 5);
                    }

                    gpio_set_level(PIN_CE, 1);
                    vTaskDelay(pdMS_TO_TICKS(5));

                    uint8_t rpd_after = nrf_read_reg(RPD);
                    uint8_t fifo_after = nrf_read_reg(FIFO_STATUS);
                    int irq_level_after = gpio_get_level(PIN_IRQ);
                    //ESP_LOGW(TAG, "NRF24: recovery done RPD=%d fifo=0x%02X PIN_IRQ after=%d", rpd_after, fifo_after, irq_level_after);
                } else {
                    //ESP_LOGI(TAG, "NRF24: recent recovery performed, skipping additional recovery");
                }
            }
        }

        /* Quick diagnostic: if FIFO has data but PIN_IRQ is not asserted, log and reinstall handler */
        {
            uint8_t fifo_stat = nrf_read_reg(FIFO_STATUS);
            int irq_level = gpio_get_level(PIN_IRQ);
            if (!(fifo_stat & 0x01) && irq_level == 1) {
                ESP_LOGW(TAG, "NRF24: FIFO has data but PIN_IRQ not asserted (fifo=0x%02X). PIN_IRQ level=%d. Reinstalling IRQ handler.",
                         fifo_stat, irq_level);
                /* attempt a mitigation: reinstall ISR and allow the next loop to drain if pin asserts */
                gpio_isr_handler_remove(PIN_IRQ);
                gpio_isr_handler_add(PIN_IRQ, nrf_irq_isr, NULL);
            }
        }

        /* Immediate watchdog: if no packet recently, attempt recovery (independent of periodic logs) */
        if (nrf_last_packet_tick != 0 && (xTaskGetTickCount() - nrf_last_packet_tick) > pdMS_TO_TICKS(NRF24_RECOVER_TIMEOUT_MS)) {
            /* avoid hammering recovery; apply cooldown */
            if ((xTaskGetTickCount() - nrf_last_recover_tick) > pdMS_TO_TICKS(NRF24_RECOVER_COOLDOWN_MS)) {
                nrf_recover_count++;
                nrf_last_recover_tick = xTaskGetTickCount();
                ESP_LOGW(TAG, "NRF24: No packets for %lu ms - attempting recovery #%lu",
                         (unsigned long)((xTaskGetTickCount() - nrf_last_packet_tick) * portTICK_PERIOD_MS), (unsigned long)nrf_recover_count);

                /* quick recovery: toggle CE and reflush registers and re-install ISR */
                int irq_level_before = gpio_get_level(PIN_IRQ);
                ESP_LOGI(TAG, "NRF24: PIN_IRQ level before recovery = %d", irq_level_before);

                gpio_set_level(PIN_CE, 0);
                vTaskDelay(pdMS_TO_TICKS(10));
                nrf_flush_rx();
                nrf_write_reg(STATUS, 0x70);
                nrf_write_reg(RF_CH, 76);

                /* Reinstall ISR handler as an extra mitigation */
                gpio_isr_handler_remove(PIN_IRQ);
                gpio_isr_handler_add(PIN_IRQ, nrf_irq_isr, NULL);

                /* Aggressive mitigation every 3 recoveries: power-cycle module */
                if ((nrf_recover_count % 3) == 0) {
                    ESP_LOGW(TAG, "NRF24: performing POWER-CYCLE as aggressive mitigation (recover #%lu)", (unsigned long)nrf_recover_count);
                    gpio_set_level(PIN_CE, 0);
                    /* power off (active-low P-channel) */
                    gpio_set_level(PIN_PWR, 1);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    /* power on */
                    gpio_set_level(PIN_PWR, 0);
                    vTaskDelay(pdMS_TO_TICKS(200));

                    /* reapply essential registers */
                    nrf_flush_rx();
                    nrf_write_reg(STATUS, 0x70);
                    nrf_write_reg(RF_CH, 76);
                    nrf_write_reg(RF_SETUP, 0x06);
                    nrf_write_reg(RX_PW_P0, 5);
                    nrf_write_buf(RX_ADDR_P0, NRF24_ADDR, 5);
                }

                gpio_set_level(PIN_CE, 1);
                vTaskDelay(pdMS_TO_TICKS(5));

                uint8_t rpd_after = nrf_read_reg(RPD);
                uint8_t fifo_after = nrf_read_reg(FIFO_STATUS);
                int irq_level_after = gpio_get_level(PIN_IRQ);
                ESP_LOGW(TAG, "NRF24: recovery done RPD=%d fifo=0x%02X PIN_IRQ after=%d", rpd_after, fifo_after, irq_level_after);
            } else {
                //ESP_LOGI(TAG, "NRF24: recent recovery performed, skipping additional recovery");
            }
        }

        /* Small delay to avoid tight loop on spurious notifications */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}