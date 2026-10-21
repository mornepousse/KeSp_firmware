"""Per-second buckets, aggregated into windows (report) or the last N seconds (TUI).

Everything is keyed by the HOST arrival time of the TX record (halves) or the
RX record (dongle), in whole seconds since the run's t0. Latency is not stored
as a number: a bucket keeps the (tx_t, rx_t) device-time pair and the clock
fit that owned it, and the window asks the fit at summary time — so every
latency of a window uses the BEST (latest) fit, not the poor early one.
"""

from __future__ import annotations

from dataclasses import dataclass, field

NS = 1_000_000_000
RXF_DUP = 0x02
TXF_NOSEQ = 0x02
PAIRS_PER_SEC = 64      # latency samples kept per emitter-second: a max-rate soak stays bounded


def percentile(vals: list[float], p: float) -> float | None:
    if not vals:
        return None
    s = sorted(vals)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def _usable(fit):
    """A clock map that has not been fitted yet (fewer than MIN_PAIRS pairs)
    gives absurd offsets (boot-time differences, ~1e6 ms): no map, no number."""
    return fit if fit is not None and getattr(fit, "ready", True) else None


@dataclass
class _Bucket:
    frames: int = 0
    seq_frames: int = 0
    arc: list = field(default_factory=lambda: [0] * 16)
    max_rt: int = 0
    timeouts: int = 0
    plos: int = 0
    matched: int = 0
    lost: int = 0
    unobserved: int = 0
    elsewhere: int = 0
    rx_frames: int = 0
    rx_dup: int = 0
    rx_rpd: int = 0
    fifo_full: int = 0
    pairs: list = field(default_factory=list)         # capped: PAIRS_PER_SEC samples, newest overwrite oldest
    pair_n: int = 0
    gaps: list = field(default_factory=list)          # (dur_ms, missing)
    asleep_ns: int = 0
    ring_drops: int = 0


@dataclass
class WindowSummary:
    emitter: int
    start_s: float
    end_s: float
    frames: int = 0
    seq_frames: int = 0
    matched: int = 0
    lost: int = 0
    unobserved: int = 0
    elsewhere: int = 0
    loss_pct: float | None = None
    rx_frames: int = 0
    dup_pct: float | None = None
    arc_hist: list = field(default_factory=lambda: [0] * 16)
    retry_mean: float | None = None
    max_rt: int = 0
    timeouts: int = 0
    plos: int = 0
    rpd_strong_pct: float | None = None
    fifo_full: int = 0
    lat_n: int = 0
    lat_p50: float | None = None
    lat_p95: float | None = None
    lat_p99: float | None = None
    gaps: int = 0
    gap_max_ms: float = 0.0
    asleep_s: float = 0.0
    ring_drops: int = 0

    @property
    def rate(self) -> float:
        span = max(self.end_s - self.start_s, 1e-9)
        return self.frames / span


@dataclass
class DongleSummary:
    start_s: float
    end_s: float
    frames: int = 0
    dups: int = 0
    drops: int = 0
    channel: int | None = None


class Stats:
    def __init__(self, t0_ns: int = 0) -> None:
        self.t0_ns = t0_ns
        self._b: dict[int, dict[int, _Bucket]] = {}
        self._d: dict[int, dict] = {}

    def _sec(self, host_ns: int) -> int:
        return max(0, (host_ns - self.t0_ns) // NS)

    def _bucket(self, e: int, host_ns: int) -> _Bucket:
        return self._b.setdefault(e, {}).setdefault(self._sec(host_ns), _Bucket())

    # -- inputs ---------------------------------------------------------------

    def add_tx(self, e: int, host_ns: int, rec: dict) -> None:
        b = self._bucket(e, host_ns)
        b.frames += 1
        if not rec.get("flags", 0) & TXF_NOSEQ:
            b.seq_frames += 1
        b.arc[min(rec.get("arc", 0), 15)] += 1
        r = rec.get("result", 0)
        if r == 1:
            b.max_rt += 1
        elif r == 2:
            b.timeouts += 1
        b.plos += rec.get("plos_delta", 0)

    def add_rx(self, e: int, host_ns: int, rec: dict) -> None:
        b = self._bucket(e, host_ns)
        b.rx_frames += 1
        if rec.get("flags", 0) & RXF_DUP:
            b.rx_dup += 1
        if rec.get("rpd", 0) & 1:
            b.rx_rpd += 1
        if rec.get("fifo_depth", 0) >= 3:
            b.fifo_full += 1
        d = self._d.setdefault(self._sec(host_ns), {"frames": 0, "dups": 0, "drops": 0, "channel": None})
        if rec.get("channel") is not None:
            d["channel"] = rec["channel"]

    def outcome(self, e: int, host_ns: int, kind: str) -> None:
        b = self._bucket(e, host_ns)
        if kind == "recovered":
            b.lost = max(0, b.lost - 1)
            b.matched += 1
        else:
            setattr(b, kind, getattr(b, kind) + 1)

    def pair(self, e: int, host_ns: int, tx_t: float, rx_t: float, fit=None, arc: int = 0) -> None:
        b = self._bucket(e, host_ns)
        if len(b.pairs) < PAIRS_PER_SEC:
            b.pairs.append((tx_t, rx_t, fit, arc))
        else:
            b.pairs[b.pair_n % PAIRS_PER_SEC] = (tx_t, rx_t, fit, arc)
        b.pair_n += 1

    def gap(self, e: int, host_ns: int, dur_ms: float, missing: int) -> None:
        self._bucket(e, host_ns).gaps.append((dur_ms, missing))

    def asleep(self, e: int, start_ns: int, end_ns: int) -> None:
        s, t = max(start_ns, self.t0_ns), end_ns
        while s < t:
            sec = self._sec(s)
            edge = self.t0_ns + (sec + 1) * NS
            chunk = min(t, edge) - s
            self._b.setdefault(e, {}).setdefault(sec, _Bucket()).asleep_ns += chunk
            s += chunk

    def ring_drops(self, e: int, host_ns: int, n: int) -> None:
        self._bucket(e, host_ns).ring_drops += n

    def dongle_periodic(self, host_ns: int, frames: int, dups: int, drops: int, channel: int) -> None:
        d = self._d.setdefault(self._sec(host_ns), {"frames": 0, "dups": 0, "drops": 0, "channel": None})
        d["frames"] += frames
        d["dups"] += dups
        d["drops"] += drops
        d["channel"] = channel

    # -- outputs --------------------------------------------------------------

    def emitters(self) -> list[int]:
        return sorted(self._b)

    def last_sec(self) -> int:
        secs = [max(v) for v in self._b.values() if v] + ([max(self._d)] if self._d else [])
        return max(secs) if secs else 0

    def window(self, e: int, start_s: float, end_s: float, fits: dict | None = None) -> WindowSummary:
        w = WindowSummary(e, start_s, end_s)
        lat: list[float] = []
        rpd = 0
        buckets = self._b.get(e, {})
        lo, hi = int(start_s), int(end_s)
        keys = [k for k in buckets if lo <= k < hi] if (hi - lo) > len(buckets) else \
               [k for k in range(lo, hi) if k in buckets]
        for k in keys:
            b = buckets[k]
            w.frames += b.frames
            w.seq_frames += b.seq_frames
            w.matched += b.matched
            w.lost += b.lost
            w.unobserved += b.unobserved
            w.elsewhere += b.elsewhere
            w.rx_frames += b.rx_frames
            w.max_rt += b.max_rt
            w.timeouts += b.timeouts
            w.plos += b.plos
            w.fifo_full += b.fifo_full
            w.ring_drops += b.ring_drops
            w.asleep_s += b.asleep_ns / NS
            for i in range(16):
                w.arc_hist[i] += b.arc[i]
            rpd += b.rx_rpd
            for dur, _miss in b.gaps:
                w.gaps += 1
                w.gap_max_ms = max(w.gap_max_ms, dur)
            for tx_t, rx_t, fit, _arc in b.pairs:
                f = _usable(fit or (fits or {}).get(e))
                if f is not None:
                    lat.append(f.excess_us(tx_t, rx_t) / 1000.0)
        dup_total = sum(buckets[k].rx_dup for k in keys)
        w.dup_pct = 100.0 * dup_total / w.rx_frames if w.rx_frames else None
        w.rpd_strong_pct = 100.0 * rpd / w.rx_frames if w.rx_frames else None
        den = w.matched + w.lost
        w.loss_pct = 100.0 * w.lost / den if den else None
        if w.frames:
            w.retry_mean = sum(i * n for i, n in enumerate(w.arc_hist)) / w.frames
        w.lat_n = len(lat)
        w.lat_p50, w.lat_p95, w.lat_p99 = (percentile(lat, p) for p in (50, 95, 99))
        return w

    def windows(self, e: int, win_s: float, end_s: float, fits: dict | None = None) -> list[WindowSummary]:
        n = int(-(-end_s // win_s))
        return [self.window(e, i * win_s, (i + 1) * win_s, fits) for i in range(n)]

    def lat_series(self, e: int, from_s: int, to_s: int, fits: dict | None = None) -> list[float | None]:
        """Median excess delay per second over [from_s, to_s)."""
        out: list[float | None] = []
        buckets = self._b.get(e, {})
        for k in range(from_s, to_s):
            b = buckets.get(k)
            vals = []
            if b:
                for tx_t, rx_t, fit, _arc in b.pairs:
                    f = _usable(fit or (fits or {}).get(e))
                    if f is not None:
                        vals.append(f.excess_us(tx_t, rx_t) / 1000.0)
            out.append(percentile(vals, 50))
        return out

    def arc_residuals(self, e: int, start_s: float, end_s: float, fits: dict | None = None) -> dict[int, tuple[float, int]]:
        """{arc k >= 1: (median residual of arc=k frames minus that of arc=0, in
        us; n)} over the clock-mapped pairs of [start_s, end_s). Empty without
        a clock map or without arc=0 frames."""
        res: dict[int, list[float]] = {}
        for k, b in self._b.get(e, {}).items():
            if not start_s <= k < end_s:
                continue
            for tx_t, rx_t, fit, arc in b.pairs:
                f = _usable(fit or (fits or {}).get(e))
                if f is not None:
                    res.setdefault(arc, []).append(f.resid_us(tx_t, rx_t))
        if not res.get(0):
            return {}
        ref = percentile(res[0], 50)
        return {a: (percentile(v, 50) - ref, len(v)) for a, v in sorted(res.items()) if a > 0}

    def dongle_window(self, start_s: float, end_s: float) -> DongleSummary:
        d = DongleSummary(start_s, end_s)
        for k, v in self._d.items():
            if start_s <= k < end_s:
                d.frames += v["frames"]
                d.dups += v["dups"]
                d.drops += v["drops"]
                if v["channel"] is not None:
                    d.channel = v["channel"]
        return d
