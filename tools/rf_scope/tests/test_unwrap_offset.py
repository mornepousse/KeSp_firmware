"""t_us unwrapping (32-bit wrap, uptime anchor) and the clock-offset fit."""

import random

import pytest

from rftrace.offset import ClockFit
from rftrace.unwrap import Unwrapper

W = 2 ** 32


def test_unwrap_continues_across_the_32bit_wrap():
    u = Unwrapper()
    assert u.unwrap(W - 1000) == W - 1000
    assert u.unwrap(500) == W + 500
    assert u.unwrap(10_000) == W + 10_000


def test_unwrap_small_reorder_goes_back_without_moving_the_head():
    u = Unwrapper()
    u.unwrap(W - 10)
    u.unwrap(100)                       # W + 100
    assert u.unwrap(W - 5) == W - 5     # a late record from before the wrap
    assert u.unwrap(200) == W + 200


def test_anchor_gives_the_epoch_from_uptime():
    u = Unwrapper()
    # uptime 5000 s = 5e9 us ; t32 = 5e9 - 2^32
    t32 = 5_000_000_000 - W + 123_456
    assert u.anchor(t32, 5000) == 5_000_000_000 + 123_456


def test_long_sleep_is_unwrapped_by_the_next_anchor_not_guessed_backwards():
    u = Unwrapper()
    u.anchor(1_000_000, 1)                       # t=1 s
    u.mark_sleep()
    # 1 h asleep = 3.6e9 us > 2^31: a signed guess would go backwards
    t = 3_600_000_000 + 1_000_000
    assert u.unwrap(t % W) == t                  # forward-only after a sleep
    assert u.anchor(t % W + 10, 3601) == t + 10


def test_anchor_survives_uptime_truncation():
    u = Unwrapper()
    # uptime_s is floor(t/1e6): t = 7.9999e6 -> uptime 7
    assert u.anchor(7_999_900, 7) == 7_999_900
    assert u.anchor(8_000_100, 8) == 8_000_100


def make_pairs(n, offset=5_000_000_000, drift_ppm=30.0, base=800.0, noise=1500.0,
               outlier_frac=0.05, span_s=600, seed=1):
    rnd = random.Random(seed)
    pairs, lat = [], []
    for _ in range(n):
        x = rnd.uniform(0, span_s) * 1e6
        d = base + rnd.expovariate(1 / noise)
        if rnd.random() < outlier_frac:
            d += rnd.uniform(20_000, 120_000)
        y = offset + x * (1 + drift_ppm * 1e-6) + d
        pairs.append((x, y))
        lat.append(d)
    return pairs, lat


def test_fit_recovers_drift_and_gives_latency_over_the_floor():
    pairs, lat = make_pairs(1500)
    f = ClockFit()
    for x, y in pairs:
        f.add(x, y)
    assert f.ready
    assert abs(f.slope - (1 + 30e-6)) < 6e-6
    got = sorted(f.excess_us(x, y) for x, y in pairs)
    true = sorted(d - min(lat) for d in lat)
    # medians agree within 0.3 ms although the absolute offset is unknown
    assert abs(got[len(got) // 2] - true[len(true) // 2]) < 300
    assert min(got) >= -1.0 or got[0] > -3000
    assert 100 < f.jitter_us < 3000


def test_fit_is_robust_to_gross_outliers():
    pairs, _ = make_pairs(800, outlier_frac=0.25, seed=7)
    f = ClockFit()
    for x, y in pairs:
        f.add(x, y)
    assert abs(f.slope - (1 + 30e-6)) < 10e-6


def test_fit_not_ready_before_min_pairs_and_short_span_has_unit_slope():
    f = ClockFit()
    for i in range(5):
        f.add(i * 1e6, 9e9 + i * 1e6 + 700)
    assert not f.ready
    for i in range(5, 12):
        f.add(i * 1e6, 9e9 + i * 1e6 + 700)
    assert f.ready and f.slope == 1.0


def test_fit_memory_is_bounded_and_still_spans_the_run():
    f = ClockFit(max_pts=200)
    for i in range(20000):
        x = i * 1e5
        f.add(x, 3e9 + x * (1 + 20e-6) + 500)
    assert f.n_kept <= 200
    assert f.n == 20000
    assert abs(f.slope - (1 + 20e-6)) < 3e-6
