/* Backend de l'horloge host contrôlable — voir host_clock.h.
 * Définit l'unique esp_timer_get_time() du runner de test. */
#include "host_clock.h"
#include "esp_timer.h"

static int64_t s_now_us = 0;

int64_t esp_timer_get_time(void)       { return s_now_us; }
void    host_clock_reset(void)         { s_now_us = 0; }
void    host_clock_advance_ms(uint32_t ms) { s_now_us += (int64_t)ms * 1000; }
