/* Math d'animation LED — extrait de led_strip_anim.c pour être testable
 * host-side sans les dépendances hardware (led_strip driver, LVGL round_ui,
 * freertos, esp_timer). led_strip_anim.c inclut ce header et appelle ces
 * fonctions ; les constantes y sont la source unique de vérité. */
#pragma once
#include <stdint.h>

#define LED_STRIP_FRAME_MS  20    /* période de frame (50 FPS) */
#define REACTIVE_ATTACK_MS  100   /* plein éclat maintenu après une frappe */
#define REACTIVE_DECAY_MS   500   /* fondu jusqu'à 0 sur cette durée */
#define KPM_BAR_MAX         400   /* KPM qui allume toutes les LEDs */

/* Luminosité réactive selon le temps écoulé depuis la dernière frappe :
 * 255 pendant l'attaque, décroissance linéaire jusqu'à 0 à REACTIVE_DECAY_MS,
 * puis 0. */
uint8_t led_reactive_brightness(uint32_t elapsed_ms);

/* Nombre de LEDs allumées pour un KPM donné (0..num_leds, clampé). */
uint8_t led_kpm_bar_lit(uint32_t kpm, uint8_t num_leds);
