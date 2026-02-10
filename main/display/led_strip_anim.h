/**
 * @file led_strip_anim.h
 * @brief WS2812 LED strip animations for keyboard
 */

#ifndef LED_STRIP_ANIM_H
#define LED_STRIP_ANIM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LED strip configuration */
#define LED_STRIP_GPIO      4
#define LED_STRIP_NUM_LEDS  17  /* Adjust to your strip length */

/* Animation types */
typedef enum {
    LED_ANIM_OFF = 0,
    LED_ANIM_STATIC,        /* Static color */
    LED_ANIM_BREATHE,       /* Breathing effect */
    LED_ANIM_RAINBOW,       /* Rainbow cycle */
    LED_ANIM_CHASE,         /* Chase effect */
    LED_ANIM_REACTIVE,      /* React to keypresses */
    LED_ANIM_KPM_BAR,       /* KPM visualization */
    LED_ANIM_MAX
} led_anim_type_t;

/**
 * @brief Initialize LED strip
 * @return ESP_OK on success
 */
esp_err_t led_strip_anim_init(void);

/**
 * @brief Deinitialize LED strip
 */
void led_strip_anim_deinit(void);

/**
 * @brief Set animation type
 * @param anim Animation type
 */
void led_strip_set_animation(led_anim_type_t anim);

/**
 * @brief Get current animation type
 */
led_anim_type_t led_strip_get_animation(void);

/**
 * @brief Run a test sequence (R/G/B) on the strip (blocking)
 * Useful for startup diagnostics
 */
void led_strip_test(void);

/**
 * @brief Set static color (for LED_ANIM_STATIC)
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
void led_strip_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set brightness (0-255)
 */
void led_strip_set_brightness(uint8_t brightness);

/**
 * @brief Notify of keypress (for reactive animation)
 */
void led_strip_notify_keypress(void);

/**
 * @brief Update animation (call periodically from task)
 */
void led_strip_update(void);

/**
 * @brief Start LED animation task
 */
void led_strip_start_task(void);

#ifdef __cplusplus
}
#endif

#endif /* LED_STRIP_ANIM_H */
