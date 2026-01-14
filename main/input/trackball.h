/**
 * @file trackball.h
 * @brief Driver for ICSH044A trackball module with RGB LED
 */

#ifndef TRACKBALL_H
#define TRACKBALL_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Trackball hardware configuration */
typedef struct {
    gpio_num_t btn;     /* Button GPIO */
    gpio_num_t up;      /* Up direction GPIO */
    gpio_num_t down;    /* Down direction GPIO */
    gpio_num_t left;    /* Left direction GPIO */
    gpio_num_t right;   /* Right direction GPIO */
    gpio_num_t led_red;   /* Red LED GPIO */
    gpio_num_t led_green; /* Green LED GPIO */
    gpio_num_t led_blue;  /* Blue LED GPIO */
    gpio_num_t led_white; /* White LED GPIO */
} trackball_config_t;

/** Trackball movement delta */
typedef struct {
    int8_t dx;      /* Horizontal movement (-127 to 127) */
    int8_t dy;      /* Vertical movement (-127 to 127) */
    bool button;    /* Button pressed */
} trackball_state_t;

/**
 * @brief Initialize the trackball
 * @param config Hardware configuration
 * @return true if successful
 */
bool trackball_init(const trackball_config_t *config);

/**
 * @brief Get current trackball state and reset counters
 * @param state Output state structure
 * @return true if there was movement or button change
 */
bool trackball_get_state(trackball_state_t *state);

/**
 * @brief Check if button is currently pressed
 * @return true if pressed
 */
bool trackball_button_pressed(void);

/**
 * @brief Set LED color (RGB)
 * @param r Red intensity (0-255)
 * @param g Green intensity (0-255)
 * @param b Blue intensity (0-255)
 */
void trackball_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Set white LED brightness
 * @param brightness 0-255
 */
void trackball_set_white(uint8_t brightness);

/**
 * @brief Turn off all LEDs
 */
void trackball_leds_off(void);

/**
 * @brief Get default configuration for VERSION_1 board
 * @return Default trackball config
 */
trackball_config_t trackball_get_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* TRACKBALL_H */
