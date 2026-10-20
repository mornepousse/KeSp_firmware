"""Per-window statistics."""

from rftrace.stats import Stats, percentile

MS = 1_000_000
S = 1_000_000_000


class FlatFit:                      # latency = rx_t - tx_t - 1000 us
    def excess_us(self, x, y):
        return y - x - 1000


def feed(st, e, sec, n, *, arc=0, result=0):
    for i in range(n):
        st.add_tx(e, sec * S + i * MS, {"arc": arc, "result": result, "flags": 0, "plos_delta": 0})


def test_percentile_interpolates():
    assert percentile([1, 2, 3, 4], 50) == 2.5
    assert percentile([], 50) is None
    assert percentile([5], 99) == 5


def test_window_sums_frames_loss_and_retries():
    st = Stats(t0_ns=0)
    feed(st, 0, 1, 90)
    feed(st, 0, 2, 10, arc=3)
    feed(st, 0, 3, 2, result=1, arc=15)
    for _ in range(95):
        st.outcome(0, 1 * S, "matched")
    for _ in range(5):
        st.outcome(0, 2 * S, "lost")
    w = st.window(0, 0, 60)
    assert w.frames == 102
    assert w.matched == 95 and w.lost == 5
    assert abs(w.loss_pct - 5.0) < 1e-9
    assert w.max_rt == 2
    assert w.arc_hist[0] == 90 and w.arc_hist[3] == 10 and w.arc_hist[15] == 2


def test_windows_split_at_the_boundary():
    st = Stats(t0_ns=0)
    feed(st, 0, 59, 3)
    feed(st, 0, 60, 4)
    assert st.window(0, 0, 60).frames == 3
    assert st.window(0, 60, 120).frames == 4
    assert [w.frames for w in st.windows(0, 60, end_s=121)] == [3, 4, 0]


def test_dup_rpd_and_latency_percentiles():
    st = Stats(t0_ns=0)
    fit = FlatFit()
    for i in range(100):
        st.add_rx(0, 1 * S + i * MS, {"flags": 2 if i < 4 else 0, "rpd": 1 if i < 90 else 0, "fifo_depth": 1})
        st.pair(0, (1 + i // 50) * S, i * 1000, i * 1000 + 1000 + (i + 1) * 100)   # (i+1) * 0.1 ms
    w = st.window(0, 0, 60, fits={0: fit})
    assert abs(w.dup_pct - 4.0) < 1e-9
    assert abs(w.rpd_strong_pct - 90.0) < 1e-9
    assert abs(w.lat_p50 - 5.05) < 0.01
    assert abs(w.lat_p95 - 9.505) < 0.01
    assert w.lat_p99 > w.lat_p95


def test_gaps_asleep_and_drops_accumulate():
    st = Stats(t0_ns=0)
    st.gap(0, 5 * S, 250.0, 3)
    st.gap(0, 6 * S, 120.0, 1)
    st.asleep(0, 10 * S, 25 * S)                 # spans windows with 60 s windows? no: inside
    st.asleep(0, 50 * S, 70 * S)                 # 10 s in window 0, 10 s in window 1
    st.ring_drops(0, 20 * S, 7)
    w0, w1 = st.windows(0, 60, end_s=120)
    assert w0.gaps == 2 and w0.gap_max_ms == 250.0
    assert abs(w0.asleep_s - 25.0) < 1e-6 and abs(w1.asleep_s - 10.0) < 1e-6
    assert w0.ring_drops == 7


def test_recovered_moves_a_loss_to_a_match():
    st = Stats(t0_ns=0)
    st.outcome(0, S, "lost")
    st.outcome(0, S, "recovered")
    w = st.window(0, 0, 60)
    assert w.lost == 0 and w.matched == 1


def test_dongle_series():
    st = Stats(t0_ns=0)
    st.dongle_periodic(2 * S, frames=40, dups=1, drops=2, channel=79)
    st.dongle_periodic(3 * S, frames=10, dups=0, drops=0, channel=79)
    d = st.dongle_window(0, 60)
    assert d.frames == 50 and d.dups == 1 and d.drops == 2 and d.channel == 79


def test_latency_samples_per_second_are_capped():
    from rftrace.stats import PAIRS_PER_SEC
    st = Stats(t0_ns=0)
    for i in range(PAIRS_PER_SEC * 5):
        st.pair(0, S, i, i + 1500)
    assert len(st._b[0][1].pairs) == PAIRS_PER_SEC
