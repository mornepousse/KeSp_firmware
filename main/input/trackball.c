/**
 * @file trackball.c
 * @brief Driver for ICSH044A trackball module with RGB LED
 * 
 * The ICSH044A uses quadrature encoding on 4 direction pins.
 * Each pulse on a direction pin indicates movement in that direction.
 */

#include "trackball.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "TRACKBALL";

static trackball_config_t s_config;
static volatile int32_t s_dx = 0;
static volatile int32_t s_dy = 0;
static volatile bool s_button_state = false;
static bool s_initialized = false;

/* ISR handlers for direction pins */
static void IRAM_ATTR trackball_isr_up(void *arg)
{
    s_dy--;
}

static void IRAM_ATTR trackball_isr_down(void *arg)
{
    s_dy++;
}

static void IRAM_ATTR trackball_isr_left(void *arg)
{
    s_dx--;
}

static void IRAM_ATTR trackball_isr_right(void *arg)
{
    s_dx++;
}

static void IRAM_ATTR trackball_isr_button(void *arg)
{
    s_button_state = !gpio_get_level(s_config.btn);
}

/* Initialize LEDC for PWM on LEDs */
static void trackball_init_leds(void)
{
    /* LEDC timer configuration */
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    /* Configure LED channels */
    ledc_channel_config_t channels[] = {
        { .gpio_num = s_config.led_red,   .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_1, .duty = 0, .hpoint = 0 },
        { .gpio_num = s_config.led_green, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1, .duty = 0, .hpoint = 0 },
        { .gpio_num = s_config.led_blue,  .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_2, .timer_sel = LEDC_TIMER_1, .duty = 0, .hpoint = 0 },
        { .gpio_num = s_config.led_white, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_3, .timer_sel = LEDC_TIMER_1, .duty = 0, .hpoint = 0 },
    };

    for (int i = 0; i < 4; i++) {
        if (channels[i].gpio_num != GPIO_NUM_NC) {
            ledc_channel_config(&channels[i]);
        }
    }
}

bool trackball_init(const trackball_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config");
        return false;
    }

    s_config = *config;

    ESP_LOGI(TAG, "Initializing ICSH044A trackball");
    ESP_LOGI(TAG, "  BTN=%d UP=%d DOWN=%d LEFT=%d RIGHT=%d", 
             config->btn, config->up, config->down, config->left, config->right);
    ESP_LOGI(TAG, "  LED R=%d G=%d B=%d W=%d",
             config->led_red, config->led_green, config->led_blue, config->led_white);

    /* Configure direction pins with pull-up and interrupt on falling edge */
    gpio_config_t dir_conf = {
        .pin_bit_mask = (1ULL << config->up) | (1ULL << config->down) |
                        (1ULL << config->left) | (1ULL << config->right),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&dir_conf);

    /* Configure button pin */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << config->btn),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&btn_conf);

    /* Install ISR service */
    gpio_install_isr_service(0);

    /* Attach ISR handlers */
    gpio_isr_handler_add(config->up, trackball_isr_up, NULL);
    gpio_isr_handler_add(config->down, trackball_isr_down, NULL);
    gpio_isr_handler_add(config->left, trackball_isr_left, NULL);
    gpio_isr_handler_add(config->right, trackball_isr_right, NULL);
    gpio_isr_handler_add(config->btn, trackball_isr_button, NULL);

    /* Initialize LEDs */
    trackball_init_leds();

    s_initialized = true;
    ESP_LOGI(TAG, "Trackball initialized");

    /* Show a brief LED test */
    trackball_set_rgb(50, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    trackball_set_rgb(0, 50, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    trackball_set_rgb(0, 0, 50);
    vTaskDelay(pdMS_TO_TICKS(100));
    trackball_leds_off();

    return true;
}

bool trackball_get_state(trackball_state_t *state)
{
    if (!s_initialized || state == NULL) return false;

    /* Atomically read and clear counters */
    portDISABLE_INTERRUPTS();
    int32_t dx = s_dx;
    int32_t dy = s_dy;
    s_dx = 0;
    s_dy = 0;
    bool btn = s_button_state;
    portENABLE_INTERRUPTS();

    /* Clamp to int8_t range */
    if (dx > 127) dx = 127;
    if (dx < -127) dx = -127;
    if (dy > 127) dy = 127;
    if (dy < -127) dy = -127;

    state->dx = (int8_t)dx;
    state->dy = (int8_t)dy;
    state->button = btn;

    return (dx != 0 || dy != 0);
}

bool trackball_button_pressed(void)
{
    if (!s_initialized) return false;
    return !gpio_get_level(s_config.btn);
}

void trackball_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) return;

    if (s_config.led_red != GPIO_NUM_NC) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, r);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (s_config.led_green != GPIO_NUM_NC) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, g);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    }
    if (s_config.led_blue != GPIO_NUM_NC) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, b);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    }
}

void trackball_set_white(uint8_t brightness)
{
    if (!s_initialized) return;

    if (s_config.led_white != GPIO_NUM_NC) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, brightness);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
    }
}

void trackball_leds_off(void)
{
    trackball_set_rgb(0, 0, 0);
    trackball_set_white(0);
}

trackball_config_t trackball_get_default_config(void)
{
    trackball_config_t cfg = {
        .btn = GPIO_NUM_4,
        .right = GPIO_NUM_5,
        .left = GPIO_NUM_6,
        .down = GPIO_NUM_7,
        .up = GPIO_NUM_15,
        .led_white = GPIO_NUM_16,
        .led_green = GPIO_NUM_17,
        .led_red = GPIO_NUM_18,
        .led_blue = GPIO_NUM_8,
    };
    return cfg;
}
