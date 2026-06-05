#include "rf_line_test.h"
#include "board_rf.h"
#include "rf_driver.h"        /* rf_radio_cfg_t */
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rf_line_test";

void rf_line_test_run(void)
{
    rf_radio_cfg_t l = board_rf_left_cfg();
    rf_radio_cfg_t r = board_rf_right_cfg();

    const struct { int gpio; const char *name; } ln[] = {
        { l.pin_mosi, "MOSI"   }, { l.pin_miso, "MISO"  }, { l.pin_sck, "SCK"    },
        { l.pin_csn,  "N1_CSN" }, { l.pin_ce,   "N1_CE" }, { l.pin_irq, "N1_IRQ" },
        { r.pin_csn,  "N2_CSN" }, { r.pin_ce,   "N2_CE" }, { r.pin_irq, "N2_IRQ" },
    };
    const int N = (int)(sizeof(ln) / sizeof(ln[0]));

    ESP_LOGW(TAG, "=== NRF line short test (%d lines, bidirectional) ===", N);
    int shorts = 0;
    for (int a = 0; a < N; a++) {
        for (int b = a + 1; b < N; b++) {
            /* drive a HIGH, b = input + pulldown → reads 1 only if a forces it */
            gpio_reset_pin(ln[a].gpio); gpio_reset_pin(ln[b].gpio);
            gpio_set_direction(ln[a].gpio, GPIO_MODE_OUTPUT);
            gpio_set_direction(ln[b].gpio, GPIO_MODE_INPUT);
            gpio_set_pull_mode(ln[b].gpio, GPIO_PULLDOWN_ONLY);
            gpio_set_level(ln[a].gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(2));
            int hi = gpio_get_level(ln[b].gpio);

            /* drive a LOW, b = input + pullup → reads 0 only if a forces it */
            gpio_set_pull_mode(ln[b].gpio, GPIO_PULLUP_ONLY);
            gpio_set_level(ln[a].gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(2));
            int lo = gpio_get_level(ln[b].gpio);

            /* Real short only if b follows a in BOTH directions (pull-up immune). */
            if (hi == 1 && lo == 0) {
                ESP_LOGW(TAG, "SHORT: %s(%d) <-> %s(%d)",
                         ln[a].name, ln[a].gpio, ln[b].name, ln[b].gpio);
                shorts++;
            }
        }
    }
    for (int i = 0; i < N; i++) gpio_reset_pin(ln[i].gpio);   /* release for radio init */

    if (shorts == 0) ESP_LOGW(TAG, "no shorts detected among NRF lines");
    ESP_LOGW(TAG, "=== end (%d short(s)) ===", shorts);
}
