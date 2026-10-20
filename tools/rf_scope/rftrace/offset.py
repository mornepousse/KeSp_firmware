"""Clock map between a half's timeline and the dongle's, fitted on matched pairs.

x = the half's TX stamp, y = the dongle's RX stamp of the SAME frame (both
unwrapped device microseconds). y = c + d * x + delay, where `delay` is what
the tool wants (airtime + ACK path + the time before the dongle reads its
FIFO) and c is the unknown difference between the two boot times.

c and the true minimum delay cannot be separated without a common clock, so:

* slope d: Theil-Sen (median of pairwise slopes) on an evenly spread subsample
  — the drift between two crystals, tens of ppm, robust to ms-scale outliers;
* intercept: the MEDIAN residual -> `jitter_us` is the MAD of the residuals
  around it (the spread of the delay);
* `floor_us`: the 1st percentile of the residuals. `excess_us(x, y)` is the
  residual minus that floor: how much LATER than the fastest frames this one
  was seen. It is not an absolute one-way latency (the floor, a few hundred
  microseconds of airtime + SPI, is unknowable here); see README.

Memory is bounded: when `max_pts` pairs are kept, every second one is dropped
and the sampling stride doubles, so the kept pairs always span the whole run.
"""

from __future__ import annotations

import bisect
import statistics

MIN_PAIRS = 8
SLOPE_MIN_SPAN_US = 5_000_000


def _pct(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return 0.0
    k = (len(sorted_vals) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


class ClockFit:
    def __init__(self, max_pts: int = 2000, refit_every: int = 25, min_pairs: int = MIN_PAIRS) -> None:
        self.max_pts = max_pts
        self.refit_every = refit_every
        self.min_pairs = min_pairs
        self._xs: list[float] = []
        self._ys: list[float] = []
        self._stride = 1
        self._skip = 0
        self.n = 0
        self._since = 0
        self.slope = 1.0
        self.x0 = 0.0
        self.y0 = 0.0
        self.jitter_us = 0.0
        self.floor_resid = 0.0

    @property
    def n_kept(self) -> int:
        return len(self._xs)

    @property
    def ready(self) -> bool:
        return self.n >= self.min_pairs

    def add(self, x: float, y: float) -> None:
        self.n += 1
        self._since += 1
        if self._skip == 0:
            i = bisect.bisect_right(self._xs, x)
            self._xs.insert(i, x)
            self._ys.insert(i, y)
            if len(self._xs) > self.max_pts:
                self._xs = self._xs[::2]
                self._ys = self._ys[::2]
                self._stride *= 2
            self._skip = self._stride - 1
        else:
            self._skip -= 1
        if self.n == self.min_pairs or (self.n > self.min_pairs and self._since >= self.refit_every):
            self.refit()

    def refit(self) -> None:
        self._since = 0
        xs, ys = self._xs, self._ys
        if len(xs) < 2:
            return
        span = xs[-1] - xs[0]
        slope = 1.0
        if span >= SLOPE_MIN_SPAN_US:
            m = len(xs)
            idx = list(range(m)) if m <= 160 else [round(i * (m - 1) / 159) for i in range(160)]
            min_dx = span / 8
            slopes = []
            for a in range(len(idx)):
                xa, ya = xs[idx[a]], ys[idx[a]]
                for b in range(a + 1, len(idx)):
                    dx = xs[idx[b]] - xa
                    if dx >= min_dx:
                        slopes.append((ys[idx[b]] - ya) / dx)
            if slopes:
                slope = statistics.median(slopes)
        self.slope = slope
        self.x0 = xs[len(xs) // 2]
        res0 = [y - slope * (x - self.x0) for x, y in zip(xs, ys)]
        self.y0 = statistics.median(res0)
        res = sorted(r - self.y0 for r in res0)
        self.jitter_us = statistics.median(abs(r) for r in res)
        self.floor_resid = _pct(res, 1.0)

    def predict(self, x: float) -> float:
        return self.y0 + self.slope * (x - self.x0)

    def resid_us(self, x: float, y: float) -> float:
        return y - self.predict(x)

    def excess_us(self, x: float, y: float) -> float:
        """Delay over the fastest frames of the run (about 0 for the best 1 %)."""
        return self.resid_us(x, y) - self.floor_resid
