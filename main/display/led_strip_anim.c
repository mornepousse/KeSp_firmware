/**
 * @file led_strip_anim.c
 * @brief WS2812 LED strip animations for keyboard
 * 
 * Uses espressif/led_strip component.
 */

#include "led_strip_anim.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

#define TAG "LED_STRIP"

#ifndef LED_STRIP_FRAME_MS
#define LED_STRIP_FRAME_MS 20  /* 50 FPS */
#endif

/* LED strip handle */
static led_strip_handle_t led_strip = NULL;
static bool strip_initialized = false;

/* Animation state */
static led_anim_type_t current_anim = LED_ANIM_RAINBOW;
static uint8_t static_r = 0, static_g = 100, static_b = 255;
static uint8_t brightness = 128;
static uint32_t anim_step = 0;
static TickType_t last_keypress_tick = 0;
static uint8_t reactive_brightness = 0;

/* ============ Color Helpers ============ */

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region, remainder, p, q, t;
    
    if (s == 0) {
        *r = *g = *b = v;
        return;
    }
    
    region = h / 43;
    remainder = (h - (region * 43)) * 6;
    
    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

static void set_led(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!strip_initialized || !led_strip) return;
    
    /* Apply brightness */
    r = (r * brightness) >> 8;
    g = (g * brightness) >> 8;
    b = (b * brightness) >> 8;
    
    led_strip_set_pixel(led_strip, index, r, g, b);
}

static void clear_all(void)
{
    if (!strip_initialized || !led_strip) return;
    led_strip_clear(led_strip);
}

static void show(void)
{
    if (!strip_initialized || !led_strip) return;
    led_strip_refresh(led_strip);
}

/* ============ Animations ============ */

static void anim_off(void)
{
    clear_all();
}

static void anim_static(void)
{
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        set_led(i, static_r, static_g, static_b);
    }
}

static void anim_breathe(void)
{
    /* Sine wave breathing */
    float phase = (float)anim_step / 100.0f;
    float breath = (sinf(phase) + 1.0f) / 2.0f;
    uint8_t level = (uint8_t)(breath * 255);
    
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        set_led(i, (static_r * level) >> 8, (static_g * level) >> 8, (static_b * level) >> 8);
    }
    anim_step++;
}

static void anim_rainbow(void)
{
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        uint8_t r, g, b;
        uint16_t hue = (anim_step + (i * 256 / LED_STRIP_NUM_LEDS)) % 256;
        hsv_to_rgb(hue, 255, 255, &r, &g, &b);
        set_led(i, r, g, b);
    }
    anim_step = (anim_step + 2) % 256;
}

static void anim_chase(void)
{
    clear_all();
    int pos = anim_step % LED_STRIP_NUM_LEDS;
    
    /* Trail effect */
    for (int i = 0; i < 3; i++) {
        int idx = (pos - i + LED_STRIP_NUM_LEDS) % LED_STRIP_NUM_LEDS;
        uint8_t fade = 255 - (i * 80);
        set_led(idx, (static_r * fade) >> 8, (static_g * fade) >> 8, (static_b * fade) >> 8);
    }
    anim_step++;
}

static void anim_reactive(void)
{
    /* Decay reactive brightness */
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (now - last_keypress_tick) * portTICK_PERIOD_MS;
    
    if (elapsed_ms < 100) {
        reactive_brightness = 255;
    } else if (elapsed_ms < 500) {
        reactive_brightness = 255 - ((elapsed_ms - 100) * 255 / 400);
    } else {
        reactive_brightness = 0;
    }
    
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        uint8_t r, g, b;
        uint16_t hue = (i * 256 / LED_STRIP_NUM_LEDS + anim_step) % 256;
        hsv_to_rgb(hue, 255, reactive_brightness, &r, &g, &b);
        set_led(i, r, g, b);
    }
    
    if (reactive_brightness > 0) {
        anim_step = (anim_step + 5) % 256;
    }
}

static void anim_kpm_bar(void)
{
    /* Show KPM as a bar graph */
    extern uint32_t current_kpm;  /* From round_ui.c */
    
    /* Map 0-400 KPM to 0-NUM_LEDS */
    int lit_leds = (current_kpm * LED_STRIP_NUM_LEDS) / 400;
    if (lit_leds > LED_STRIP_NUM_LEDS) lit_leds = LED_STRIP_NUM_LEDS;
    
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        if (i < lit_leds) {
            /* Color gradient: blue -> green -> yellow -> red */
            uint8_t r, g, b;
            uint16_t hue = 170 - (i * 170 / LED_STRIP_NUM_LEDS);  /* Blue to red */
            hsv_to_rgb(hue, 255, 255, &r, &g, &b);
            set_led(i, r, g, b);
        } else {
            set_led(i, 0, 0, 0);
        }
    }
}

/* ============ Public API ============ */

esp_err_t led_strip_anim_init(void)
{
    if (strip_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing LED strip on GPIO %d, %d LEDs", LED_STRIP_GPIO, LED_STRIP_NUM_LEDS);
    
    /* RMT backend configuration */
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_NUM_LEDS,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10MHz */
        .flags.with_dma = false,
    };
    
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }
    
    strip_initialized = true;
    
    /* Start with all LEDs off */
    clear_all();
    show();
    
    ESP_LOGI(TAG, "LED strip initialized");
    return ESP_OK;
}

void led_strip_anim_deinit(void)
{
    if (!strip_initialized) return;
    
    clear_all();
    show();
    
    led_strip_del(led_strip);
    led_strip = NULL;
    
    strip_initialized = false;
    ESP_LOGI(TAG, "LED strip deinitialized");
}

void led_strip_set_animation(led_anim_type_t anim)
{
    if (anim >= LED_ANIM_MAX) return;
    current_anim = anim;
    anim_step = 0;
    ESP_LOGI(TAG, "Animation set to %d", anim);
}

led_anim_type_t led_strip_get_animation(void)
{
    return current_anim;
}

void led_strip_test(void)
{
    if (led_strip_anim_init() != ESP_OK) return;
    
    ESP_LOGI(TAG, "Running LED test sequence on GPIO %d with %d LEDs", LED_STRIP_GPIO, LED_STRIP_NUM_LEDS);
    
    esp_err_t ret;
    
    /* RED */
    ESP_LOGI(TAG, "TEST: Setting RED...");
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        ret = led_strip_set_pixel(led_strip, i, 255, 0, 0);
        if (ret != ESP_OK) ESP_LOGE(TAG, "set_pixel[%d] RED failed: %s", i, esp_err_to_name(ret));
    }
    ret = led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "TEST: RED refresh result: %s", esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* GREEN */
    ESP_LOGI(TAG, "TEST: Setting GREEN...");
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        ret = led_strip_set_pixel(led_strip, i, 0, 255, 0);
        if (ret != ESP_OK) ESP_LOGE(TAG, "set_pixel[%d] GREEN failed: %s", i, esp_err_to_name(ret));
    }
    ret = led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "TEST: GREEN refresh result: %s", esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* BLUE */
    ESP_LOGI(TAG, "TEST: Setting BLUE...");
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        ret = led_strip_set_pixel(led_strip, i, 0, 0, 255);
        if (ret != ESP_OK) ESP_LOGE(TAG, "set_pixel[%d] BLUE failed: %s", i, esp_err_to_name(ret));
    }
    ret = led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "TEST: BLUE refresh result: %s", esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* WHITE - test all colors */
    ESP_LOGI(TAG, "TEST: Setting WHITE...");
    for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
        ret = led_strip_set_pixel(led_strip, i, 255, 255, 255);
        if (ret != ESP_OK) ESP_LOGE(TAG, "set_pixel[%d] WHITE failed: %s", i, esp_err_to_name(ret));
    }
    ret = led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "TEST: WHITE refresh result: %s", esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* OFF */
    ESP_LOGI(TAG, "TEST: Turning OFF...");
    led_strip_clear(led_strip);
    ret = led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "TEST: OFF refresh result: %s", esp_err_to_name(ret));
    ESP_LOGI(TAG, "LED test sequence complete");
}

void led_strip_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    static_r = r;
    static_g = g;
    static_b = b;
}

void led_strip_set_brightness(uint8_t b)
{
    brightness = b;
}

void led_strip_notify_keypress(void)
{
    last_keypress_tick = xTaskGetTickCount();
    reactive_brightness = 255;
    /* In reactive mode, force immediate update? */
}

void led_strip_update(void)
{
    if (!strip_initialized) return;
    
    switch (current_anim) {
        case LED_ANIM_OFF:      anim_off(); break;
        case LED_ANIM_STATIC:   anim_static(); break;
        case LED_ANIM_BREATHE:  anim_breathe(); break;
        case LED_ANIM_RAINBOW:  anim_rainbow(); break;
        case LED_ANIM_CHASE:    anim_chase(); break;
        case LED_ANIM_REACTIVE: anim_reactive(); break;
        case LED_ANIM_KPM_BAR:  anim_kpm_bar(); break;
        default: break;
    }
    
    show();
}

/* ============ Task ============ */

static void led_strip_task(void *arg)
{
    (void)arg;
    
    if (led_strip_anim_init() != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed, task exiting");
        vTaskDelete(NULL);
        return;
    }
    
    for (;;) {
        led_strip_update();
        vTaskDelay(pdMS_TO_TICKS(LED_STRIP_FRAME_MS));
    }
}

void led_strip_start_task(void)
{
    xTaskCreatePinnedToCore(led_strip_task, "led_strip", 4096, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "LED strip task started");
}
