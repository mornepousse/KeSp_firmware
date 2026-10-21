/* nRF24 bench knobs — see rf_bench.h. PURE. */
#include "rf_bench.h"
#include <stdio.h>

uint8_t rf_bench_flags(uint8_t feature, uint8_t setup_retr,
                       uint32_t settle_us, uint32_t settle_shipped)
{
    uint8_t f = 0;
    if (feature != RF_SHIPPED_FEATURE)       f |= RF_BENCH_FEATURE;
    if (setup_retr != RF_SHIPPED_SETUP_RETR) f |= RF_BENCH_RETR;
    if (settle_us != settle_shipped)         f |= RF_BENCH_SETTLE;
    return f;
}

void rf_bench_describe(uint8_t flags, uint8_t feature, uint8_t setup_retr,
                       uint32_t settle_us, char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = 0;
    if (flags == 0) return;
    char f[16] = "", r[16] = "", s[24] = "";
    if (flags & RF_BENCH_FEATURE) snprintf(f, sizeof f, "feature=0x%02X", (unsigned)feature);
    if (flags & RF_BENCH_RETR)    snprintf(r, sizeof r, "retr=0x%02X", (unsigned)setup_retr);
    if (flags & RF_BENCH_SETTLE)  snprintf(s, sizeof s, "settle=%luus", (unsigned long)settle_us);
    snprintf(out, cap, " BENCH-RF(%s%s%s%s%s)",
             f, (f[0] && (r[0] || s[0])) ? " " : "",
             r, (r[0] && s[0]) ? " " : "", s);
}
