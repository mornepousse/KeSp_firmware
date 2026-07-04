/* Math d'animation LED pure — voir led_curve.h. Aucune dépendance hardware. */
#include "led_curve.h"

uint8_t led_reactive_brightness(uint32_t elapsed_ms)
{
    if (elapsed_ms < REACTIVE_ATTACK_MS)
        return 255;
    if (elapsed_ms < REACTIVE_DECAY_MS)
        return (uint8_t)(255 - ((elapsed_ms - REACTIVE_ATTACK_MS) * 255
                                / (REACTIVE_DECAY_MS - REACTIVE_ATTACK_MS)));
    return 0;
}

uint8_t led_kpm_bar_lit(uint32_t kpm, uint8_t num_leds)
{
    uint32_t lit = (kpm * num_leds) / KPM_BAR_MAX;
    if (lit > num_leds) lit = num_leds;
    return (uint8_t)lit;
}
