#include "half_battery.h"

uint8_t half_batt_soc_pct(uint8_t batt_dV)
{
    if (batt_dV == 0) return 0;   /* unknown / no reading */

    /* 1S Li-ion discharge curve: decivolts → SoC %. Monotonic; tunable. */
    static const struct { uint8_t dv; uint8_t pct; } lut[] = {
        { 30, 0 }, { 34, 8 }, { 36, 30 }, { 38, 60 }, { 40, 85 }, { 42, 100 },
    };
    const int n = (int)(sizeof(lut) / sizeof(lut[0]));

    if (batt_dV <= lut[0].dv)     return lut[0].pct;       /* ≤ 3.0 V → 0 % */
    if (batt_dV >= lut[n - 1].dv) return lut[n - 1].pct;   /* ≥ 4.2 V → 100 % */

    for (int i = 1; i < n; i++) {
        if (batt_dV <= lut[i].dv) {
            int dv0 = lut[i - 1].dv, dv1 = lut[i].dv;
            int p0  = lut[i - 1].pct, p1 = lut[i].pct;
            return (uint8_t)(p0 + (int)(batt_dV - dv0) * (p1 - p0) / (dv1 - dv0));
        }
    }
    return 0;   /* unreachable (clamped above) */
}
