#include "rf_probe.h"
#include "board.h"
#include "rf_driver.h"
#include "rf_slot.h"   /* channel plan */
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rf_probe";

/* Channel and suffix taken from boards/kase_dongle/board_rf.h (radio 1) and
 * comm/rf/rf_slot.h: slot 0x01 is the keyboard. They have no importance for
 * the probe itself — which only reads two registers — but they avoid leaving
 * a half-wrong config in the tree. */
#define RF_PROBE_CHANNEL      0x4C   /* 2476 MHz, dongle's keyboard slot */
#define RF_PROBE_ADDR_SUFFIX  0x01


/* Line test — looks for shorts between the radio's six signals.
 * Each pair is tested in BOTH DIRECTIONS: A is forced high with B as an input
 * pulled low, then A low with B pulled high. A short is only declared if B
 * follows A in both cases, which makes the test immune to a line's own
 * pull-up (the nRF24's IRQ is high at rest). Taken from rf_line_test.c,
 * reduced to a single radio: the Niphargus halves only carry one, whereas the
 * dongle has two and runs the test through board_rf_radio1/2_cfg().
 *
 * Resets all pins to zero on exit, so that the SPI/radio init that follows
 * can claim them. */
static void rf_probe_lines(void)
{
    const struct { int gpio; const char *name; } ln[] = {
        { BOARD_NRF_MOSI, "MOSI" }, { BOARD_NRF_MISO, "MISO" },
        { BOARD_NRF_SCK,  "SCK"  }, { BOARD_NRF_CSN,  "CSN"  },
        { BOARD_NRF_CE,   "CE"   }, { BOARD_NRF_IRQ,  "IRQ"  },
    };
    const int N = (int)(sizeof(ln) / sizeof(ln[0]));

    ESP_LOGW(TAG, "--- test de lignes (%d lignes, bidirectionnel) ---", N);
    int shorts = 0;
    for (int a = 0; a < N; a++) {
        for (int b = 0; b < N; b++) {
            if (a == b) continue;
            gpio_reset_pin(ln[a].gpio); gpio_reset_pin(ln[b].gpio);
            gpio_set_direction(ln[a].gpio, GPIO_MODE_OUTPUT);
            gpio_set_direction(ln[b].gpio, GPIO_MODE_INPUT);

            gpio_set_pull_mode(ln[b].gpio, GPIO_PULLDOWN_ONLY);
            gpio_set_level(ln[a].gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(2));
            int hi = gpio_get_level(ln[b].gpio);

            gpio_set_pull_mode(ln[b].gpio, GPIO_PULLUP_ONLY);
            gpio_set_level(ln[a].gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(2));
            int lo = gpio_get_level(ln[b].gpio);

            if (hi == 1 && lo == 0 && a < b) {
                ESP_LOGW(TAG, "PONT : %s(%d) <-> %s(%d)",
                         ln[a].name, ln[a].gpio, ln[b].name, ln[b].gpio);
                shorts++;
            }
        }
    }

    /* Resting state of each line, floating input then pulled: distinguishes a
     * free line from one nailed to a supply rail or to ground. */
    ESP_LOGW(TAG, "--- etat au repos (pu = avec pull-up, pd = avec pull-down) ---");
    for (int i = 0; i < N; i++) {
        gpio_reset_pin(ln[i].gpio);
        gpio_set_direction(ln[i].gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode(ln[i].gpio, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(2));
        int pu = gpio_get_level(ln[i].gpio);
        gpio_set_pull_mode(ln[i].gpio, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(2));
        int pd = gpio_get_level(ln[i].gpio);
        /* The nRF24's IRQ is active LOW and rests HIGH: a chip that is present
         * and powered holds it high itself, and an internal pull-down
         * (~45 kOhm) doesn't pull it down. Reading pu=1 pd=1 on this line is
         * therefore a sign of a LIVE radio, not a fault — it's even the only
         * witness of presence that the line test gives. Labeling it as a
         * short would panic for nothing. The other five lines, on the other
         * hand, are driven by the MCU and really must be free at rest. */
        const bool est_irq = (ln[i].gpio == BOARD_NRF_IRQ);
        const char *verdict = (pu == 1 && pd == 0) ? "libre"
                            : (pu == 0 && pd == 0) ? "CLOUEE A LA MASSE"
                            : (pu == 1 && pd == 1) ? (est_irq ? "tenue haute (normal : IRQ au repos)"
                                                             : "CLOUEE AU 3V3")
                            : "incoherente";
        ESP_LOGW(TAG, "%-4s (GPIO%2d) : pu=%d pd=%d -> %s",
                 ln[i].name, ln[i].gpio, pu, pd, verdict);
    }

    for (int i = 0; i < N; i++) gpio_reset_pin(ln[i].gpio);
    ESP_LOGW(TAG, "--- fin test de lignes (%d pont(s)) ---", shorts);
}

void rf_probe_run(void)
{
    rf_radio_cfg_t cfg = {
        .spi_host         = BOARD_NRF_SPI_HOST,
        .pin_mosi         = BOARD_NRF_MOSI,
        .pin_miso         = BOARD_NRF_MISO,
        .pin_sck          = BOARD_NRF_SCK,
        .clock_hz         = 8 * 1000 * 1000,
        .pin_csn          = BOARD_NRF_CSN,
        .pin_ce           = BOARD_NRF_CE,
        .pin_irq          = BOARD_NRF_IRQ,
        .channel          = RF_PROBE_CHANNEL,
        .rx_addr          = { 'K', 'a', 'S', 'e' },
        .addr_suffix      = RF_PROBE_ADDR_SUFFIX,
        .shares_bus_first = true,
    };

    ESP_LOGW(TAG, "=== probe nRF24 : sck=%d miso=%d mosi=%d csn=%d ce=%d irq=%d ===",
             cfg.pin_sck, cfg.pin_miso, cfg.pin_mosi,
             cfg.pin_csn, cfg.pin_ce, cfg.pin_irq);

    rf_probe_lines();

    static rf_radio_t radio;

#if CONFIG_KASE_HAS_RF_TX
    /* TRANSMIT trial. rf_driver_init_tx() also does the register read, so no
     * double init of the SPI bus.
     *
     * With no receiver on the channel, every send ends in MAX_RT — that's
     * expected, and it stays informative: the transmitter really did run, it
     * consumed its retransmissions, and above all THE BOARD HELD UP. That's
     * the real stake when it's powered from an FTDI probe's LDO: a voltage
     * drop under transmit spikes would show up as a reset. */
    cfg.channel     = RF_CH_HALF_LINK;
    cfg.addr_suffix = RF_ADDR_HALF_LINK;
    esp_err_t e = rf_driver_init_tx(&radio, &cfg);
    if (e != ESP_OK || !radio.present) {
        ESP_LOGW(TAG, "=== PAS DE REPONSE (err=%s) ===", esp_err_to_name(e));
        return;
    }
    ESP_LOGW(TAG, "=== LA RADIO REPOND (init PTX OK, ch=%u) ===", cfg.channel);

    const int N = 20;
    uint8_t pkt[8] = { 0xA5, 0, 0, 0, 0, 0, 0, 0 };
    int acked = 0;
    uint32_t max_rt_avant = rf_tx_max_rt_count;
    for (int i = 0; i < N; i++) {
        pkt[1] = (uint8_t)i;
        if (rf_driver_send(&radio, pkt, sizeof(pkt))) acked++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "=== rafale TX : %d/%d acquittes, %u MAX_RT ===",
             acked, N, (unsigned)(rf_tx_max_rt_count - max_rt_avant));
    ESP_LOGW(TAG, "=== la carte a survecu a la rafale ===");
#else
    esp_err_t e = rf_driver_init(&radio, &cfg);
    if (e == ESP_OK && radio.present)
        ESP_LOGW(TAG, "=== LA RADIO REPOND (init PRX OK) ===");
    else
        ESP_LOGW(TAG, "=== PAS DE REPONSE (err=%s) — voir la ligne 'probe csn=' "
                      "ci-dessus pour les valeurs lues ===", esp_err_to_name(e));
#endif
}
