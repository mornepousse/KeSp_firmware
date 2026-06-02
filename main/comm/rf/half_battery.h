#ifndef HALF_BATTERY_H
#define HALF_BATTERY_H

#include <stdint.h>

/* Convert a 1S Li-ion cell voltage in decivolts (×10: 30..42 = 3.0..4.2 V) to
 * a state-of-charge percentage (0..100).
 *
 * - batt_dV == 0 means "no reading / unknown" → returns 0.
 * - Clamped: ≤ 3.0 V → 0 %, ≥ 4.2 V → 100 %.
 * - Linearly interpolated between a small discharge-curve LUT (monotonic).
 *
 * Pure function, host-testable. Used by the half's battery telemetry
 * (en_battery_t.soc_pct). For a different chemistry or a 2S pack, tune the LUT
 * in half_battery.c (or scale batt_dV by the cell count before calling). */
uint8_t half_batt_soc_pct(uint8_t batt_dV);

#endif /* HALF_BATTERY_H */
