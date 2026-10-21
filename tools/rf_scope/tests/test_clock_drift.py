"""Clock map on long runs (piecewise drift), no map on too few pairs, and the
per-arc residual line of the report."""

import random

from rftrace.analyzer import Analyzer
from rftrace.offset import ClockFit
from rftrace.report import render
from rftrace.stats import Stats

S = 1_000_000_000
BOOT = 5_000_000_000


def drifting_pairs(seconds=600, rate_hz=20, seed=3):
    """Offset y - x whose RATE changes with time (2026-10-21 soak: -210 ppm for
    ~290 s, flat for ~125 s, -210 ppm again), plus 0.8 ms + exp(0.5 ms) delay."""
    rnd = random.Random(seed)
    out = []
    for i in range(seconds * rate_hz):
        t = i / rate_hz                                   # true seconds
        if t < 290:
            off = -210e-6 * t
        elif t < 415:
            off = -210e-6 * 290
        else:
            off = -210e-6 * 290 - 210e-6 * (t - 415)
        x = BOOT + t * 1e6
        y = 9e10 + x + off * 1e6 + 800 + rnd.expovariate(1 / 500)
        out.append((t, x, y))
    return out


def lat_stats(pairs, win_s):
    """p50 of every window of win_s seconds, one Stats fed with the final fit."""
    fit = ClockFit()
    st = Stats(0)
    for _t, x, y in pairs:
        fit.add(x, y)
    for t, x, y in pairs:
        st.pair(0, int(t * S), x, y, fit)
    end = int(pairs[-1][0]) + 1
    return [w.lat_p50 for w in st.windows(0, win_s, end, {0: fit}) if w.lat_n], fit


def test_long_run_with_changing_drift_gives_sub_ms_p50():
    p50s, fit = lat_stats(drifting_pairs(), 600)
    assert p50s[0] < 1.0, p50s


def test_latency_percentiles_are_stable_across_window_lengths():
    pairs = drifting_pairs()
    ref = lat_stats(pairs, 600)[0][0]
    for win in (20, 60, 120, 300):
        p50s, _ = lat_stats(pairs, win)
        assert all(abs(p - ref) < 0.4 for p in p50s), (win, p50s, ref)


def test_short_run_still_works_like_before():
    p50s, fit = lat_stats(drifting_pairs(seconds=20), 20)
    assert p50s[0] < 0.6


def test_no_clock_map_means_no_latency_number():
    fit = ClockFit()
    st = Stats(0)
    for i in range(3):                                   # cap1.jsonl: 3 pairs
        fit.add(i * 1e6, 9e11 + i * 1e6)
        st.pair(0, S, i * 1e6, 9e11 + i * 1e6, fit)
    assert not fit.ready
    w = st.window(0, 0, 60, {0: fit})
    assert w.lat_n == 0 and w.lat_p50 is None and w.lat_p99 is None
    assert st.lat_series(0, 0, 3, {0: fit}) == [None, None, None]


def test_report_prints_a_dash_and_the_reason_without_clock_map():
    an = Analyzer()
    an.t0, an.last_ns = 0, 10 * S
    fit = ClockFit()
    an.fits[0] = fit
    for i in range(3):
        an.stats.add_tx(0, S, {"arc": 1, "result": 0, "flags": 0, "plos_delta": 0})
        fit.add(i * 1e6, 9e11 + i * 1e6)
        an.stats.pair(0, S, i * 1e6, 9e11 + i * 1e6, fit, arc=1)
    text = render(an)
    lat = next(ln for ln in text.splitlines() if "latency over the floor" in ln)
    assert "p50 - ms   p95 - ms   p99 - ms" in lat
    assert "no clock map" in lat and "3 of 8" in lat


def arc_run(shift_us):
    """arc=k frames sit k * shift_us below arc=0 (first attempt received)."""
    rnd = random.Random(5)
    fit, st = ClockFit(), Stats(0)
    rows = []
    for i in range(3000):
        x = BOOT + i * 20_000.0
        arc = rnd.choice((0, 0, 0, 1, 1, 2))
        y = 7e10 + x + 800 + rnd.expovariate(1 / 300) - arc * shift_us
        fit.add(x, y)
        rows.append((i, x, y, arc))
    for i, x, y, arc in rows:
        st.pair(0, int(i * 0.02 * S), x, y, fit, arc=arc)
    return st, fit


def test_arc_residuals_show_the_first_attempt_offset():
    st, fit = arc_run(770)
    res = st.arc_residuals(0, 0, 100, {0: fit})
    assert set(res) == {1, 2}
    assert abs(res[1][0] - (-770)) < 120 and abs(res[2][0] - (-1540)) < 120
    assert res[1][1] > 100


def test_report_has_the_residual_by_retries_line_and_the_hint():
    st, fit = arc_run(770)
    an = Analyzer()
    an.t0, an.last_ns = 0, 61 * S
    an.stats, an.fits[0] = st, fit
    text = render(an)
    line = next(ln for ln in text.splitlines() if "residual by retries" in ln)
    assert "arc1 -0.7" in line or "arc1 -0.8" in line
    assert "arc2 -1.5" in line
    assert "got the first attempt" in text and "got the retransmission" in text
