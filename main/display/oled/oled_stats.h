#pragma once
#include <stdint.h>

/* Downsample hist[n] into out[out_n] bar heights 0..7 (average per bin,
   linear scale over max; max=0 -> all zero).
   Call contract: out_n <= n (downsampling). With out_n > n (upsampling)
   empty bins come out as 0 — not the intended use case. */
void oled_sparkline_bars(const uint32_t *hist, int n, uint32_t max,
                         uint8_t *out, int out_n);

/* Words/min ~= keystrokes/min / 5. */
uint32_t oled_wpm_from_kpm(uint32_t kpm);
