#include "half_power.h"

half_power_state_t half_power_next(uint32_t last_activity_ms, uint32_t now_ms)
{
    uint32_t idle = now_ms - last_activity_ms;   /* monotonic ms; wrap ~49j accepte */
    if (idle < HALF_POWER_T_THROTTLE_MS) return HALF_POWER_ACTIVE;
    if (idle < HALF_POWER_T_SLEEP_MS)    return HALF_POWER_THROTTLE;
    return HALF_POWER_SLEEP;
}

uint8_t half_power_hb_divisor(half_power_state_t state)
{
    switch (state) {
        case HALF_POWER_ACTIVE:   return 1;
        case HALF_POWER_THROTTLE: return 5;
        case HALF_POWER_SLEEP:    return 10;
        default:                  return 1;
    }
}
