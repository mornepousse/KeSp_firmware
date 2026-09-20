/* LED animation math — extracted from led_strip_anim.c to be testable
 * host-side without the hardware dependencies (led_strip driver, LVGL round_ui,
 * freertos, esp_timer). led_strip_anim.c includes this header and calls these
 * functions; the constants here are the single source of truth. */
#pragma once
#include <stdint.h>

#define LED_STRIP_FRAME_MS  20    /* frame period (50 FPS) */
#define REACTIVE_ATTACK_MS  100   /* full brightness held after a keystroke */
#define REACTIVE_DECAY_MS   500   /* fade to 0 over this duration */
#define KPM_BAR_MAX         400   /* KPM that lights up all the LEDs */

/* Reactive brightness based on time elapsed since the last keystroke:
 * 255 during the attack, linear decay to 0 at REACTIVE_DECAY_MS,
 * then 0. */
uint8_t led_reactive_brightness(uint32_t elapsed_ms);

/* Number of LEDs lit for a given KPM (0..num_leds, clamped). */
uint8_t led_kpm_bar_lit(uint32_t kpm, uint8_t num_leds);
