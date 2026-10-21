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

The drift is NOT constant over a long run. On 2026-10-21 a 10-minute soak showed
the offset falling at -210 ppm for 290 s, going flat for 125 s, then falling at
-210 ppm again (the line's slope was -150 ppm and its residual walked +-45 ms:
p50 11.8 ms against 0.7 ms on 20-s runs of the same link). So the straight line
(slope + intercept above) is only the BASELINE; what is left of the residuals is
followed by a piecewise-linear curve through the median of each ~5 s group of
pairs (`CURVE_BIN_US`, merged until `CURVE_MIN_PTS` pairs). Over a short or
sparse run there is a single group and the model is the old line.
"""

from __future__ import annotations

import bisect
import statistics

MIN_PAIRS = 8
SLOPE_MIN_SPAN_US = 5_000_000
CURVE_BIN_US = 5_000_000         # width of the elementary time bins of the curve
CURVE_MIN_PTS = 64               # a knot needs this many pairs (bins are merged until then)
BIN_CAP = 96                     # pairs kept per bin (halved when exceeded)
FLOOR_SAMPLE = 4000              # residuals used for the floor / jitter


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
        self.fitted = False
        self._bins: dict[int, list] = {}      # bin index -> [(x, y), ...]
        self._bin_med: dict[int, tuple] = {}  # bin index -> (median x, median residual, n)
        self._dirty: set[int] = set()
        self._slope_at_n = 0
        self._kx: list[float] = []            # knots of the curve
        self._ky: list[float] = []

    @property
    def n_kept(self) -> int:
        return len(self._xs)

    @property
    def ready(self) -> bool:
        """True once a map has actually been fitted (>= min_pairs pairs)."""
        return self.fitted and self.n >= self.min_pairs

    def add(self, x: float, y: float) -> None:
        self.n += 1
        self._since += 1
        k = int(x // CURVE_BIN_US)
        b = self._bins.setdefault(k, [])
        b.append((x, y))
        if len(b) > BIN_CAP:
            del b[::2]
        self._dirty.add(k)
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
        every = min(500, max(self.refit_every, self.n // 40))   # refit cost stays amortised on a soak
        if self.n == self.min_pairs or (self.n > self.min_pairs and self._since >= every):
            self.refit()

    def refit(self) -> None:
        self._since = 0
        xs, ys = self._xs, self._ys
        if len(xs) < 2:
            return
        if self._slope_at_n == 0 or self.n >= self._slope_at_n * 5 // 4:
            self._fit_baseline(xs, ys)
            self._slope_at_n = self.n
        self._fit_curve()
        res = sorted(self._sample_resid())
        med = statistics.median(res)
        # the knots are medians of bins: recentre so that the median residual is 0
        self._ky = [k + med for k in self._ky]
        self.jitter_us = statistics.median(abs(r - med) for r in res)
        self.floor_resid = _pct([r - med for r in res], 1.0)
        self.fitted = True

    def _fit_baseline(self, xs: list[float], ys: list[float]) -> None:
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
        self._dirty = set(self._bins)                    # every cached bin median used the old slope

    def _base(self, x: float) -> float:
        return self.slope * (x - self.x0)

    def _fit_curve(self) -> None:
        """Knots = (mean x, median of y - baseline) of groups of consecutive
        bins holding at least CURVE_MIN_PTS pairs; a thin last group is merged
        into the previous one."""
        for k in self._dirty:
            pts = self._bins.get(k)
            if pts:
                self._bin_med[k] = (statistics.median(p[0] for p in pts),
                                    statistics.median(p[1] - self._base(p[0]) for p in pts), len(pts))
        self._dirty = set()
        groups: list[list[tuple]] = []
        cur: list[tuple] = []
        cnt = 0
        for k in sorted(self._bin_med):
            cur.append(self._bin_med[k])
            cnt += self._bin_med[k][2]
            if cnt >= CURVE_MIN_PTS:
                groups.append(cur)
                cur, cnt = [], 0
        if cur:
            if groups:
                groups[-1].extend(cur)
            else:
                groups.append(cur)
        kx, ky = [], []
        for g in groups:
            tot = sum(b[2] for b in g)
            gx = sum(b[0] * b[2] for b in g) / tot
            acc, gy = 0, 0.0
            for v, w in sorted((b[1], b[2]) for b in g):      # weighted median of the bins' medians
                acc += w
                gy = v
                if acc >= tot / 2:
                    break
            kx.append(gx)
            ky.append(gy)
        self._kx, self._ky = kx, ky

    def _sample_resid(self) -> list[float]:
        pts = [p for b in self._bins.values() for p in b]
        step = max(1, len(pts) // FLOOR_SAMPLE)
        return [y - self._base(x) - self._curve(x) for x, y in pts[::step]]

    def _curve(self, x: float) -> float:
        kx, ky = self._kx, self._ky
        if not kx:
            return 0.0
        if x <= kx[0] or len(kx) == 1:
            return ky[0]
        if x >= kx[-1]:
            return ky[-1]
        i = bisect.bisect_right(kx, x)
        f = (x - kx[i - 1]) / (kx[i] - kx[i - 1])
        return ky[i - 1] + f * (ky[i] - ky[i - 1])

    def predict(self, x: float) -> float:
        return self._curve(x) + self._base(x)

    def resid_us(self, x: float, y: float) -> float:
        return y - self.predict(x)

    def excess_us(self, x: float, y: float) -> float:
        """Delay over the fastest frames of the run (about 0 for the best 1 %)."""
        return self.resid_us(x, y) - self.floor_resid
