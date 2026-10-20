"""TX (half) <-> RX (dongle) matching by (emitter, kind, seq) with wrap,
duplicates, reordering, NOSEQ, dongle liveness and half sleep."""

from rftrace.matcher import Matcher, MatcherConfig

MS = 1_000_000
CFG = MatcherConfig(window_ms=1000)
OFF = 7_000_000_000          # dongle clock = half clock + OFF (us)


def tx(e, seq, ms, flags=0, kind=4, result=0, arc=0, ch=82):
    return {"emitter": e, "seq": seq, "pkt_kind": kind, "t": ms * 1000, "host_ns": ms * MS,
            "result": result, "arc": arc, "flags": flags, "channel": ch, "payload_len": 6}


def rx(e, seq, ms, lat_ms=2, flags=0, kind=4, rpd=1, fifo=1):
    return {"emitter": e, "seq": seq, "pkt_kind": kind, "t": ms * 1000 + OFF + lat_ms * 1000,
            "host_ns": (ms + lat_ms) * MS, "flags": flags, "rpd": rpd, "fifo_depth": fifo,
            "channel": 82, "payload_len": 6}


def evs(out, name):
    return [e for e in out if e["ev"] == name]


def run(m, steps):
    out = []
    for kind, obs in steps:
        out += getattr(m, kind)(obs) if kind in ("tx", "rx") else []
    return out


def alive_until(m, start_ms, end_ms, step=500):
    out = []
    for t in range(start_ms, end_ms + 1, step):
        out += m.dongle_alive(t * MS)
    return out


def test_simple_match_gives_both_timestamps():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 5, 100)) + m.rx(rx(0, 5, 100))
    ma = evs(out, "matched")
    assert len(ma) == 1 and ma[0]["e"] == 0 and ma[0]["tx_t"] == 100_000
    assert ma[0]["rx_t"] == 100_000 + OFF + 2000


def test_rx_before_tx_is_claimed_when_the_tx_line_arrives_late():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.rx(rx(0, 9, 100)) + m.tx(tx(0, 9, 100))
    assert len(evs(out, "matched")) == 1
    assert not evs(m.finish(10_000 * MS), "orphan")


def test_seq_wrap_255_to_0_matches_each_frame_once():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = []
    t = 10
    for k in range(250, 262):
        s = k % 256
        out += m.tx(tx(0, s, t)) + m.rx(rx(0, s, t))
        t += 20
    assert len(evs(out, "matched")) == 12
    assert not evs(out, "dup")


def test_aliasing_after_256_frames_with_a_lost_one_does_not_mismatch():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 7, 10))                     # seq 7: never received
    t = 20
    for k in range(8, 8 + 255):                  # 255 frames, all received
        out += m.tx(tx(0, k % 256, t)) + m.rx(rx(0, k % 256, t))
        out += m.dongle_alive(t * MS)
        t += 2
    out += m.tx(tx(0, 7, t)) + m.rx(rx(0, 7, t))   # seq 7 again, new frame
    out += m.dongle_alive((t + 3) * MS)
    lost = evs(out, "lost")
    assert len(lost) == 1 and lost[0]["tx_host_ns"] == 10 * MS
    assert len(evs(out, "matched")) == 256


def test_lost_only_after_window_and_only_if_dongle_was_tracing():
    m = Matcher(CFG)
    out = alive_until(m, 0, 100)                 # dongle alive from t=0
    out += m.tx(tx(0, 1, 100))
    out += m.dongle_alive(600 * MS)
    assert not evs(out, "lost")                  # window still open
    out += m.dongle_alive(1200 * MS)
    assert len(evs(out, "lost")) == 1


def test_no_loss_when_dongle_was_not_tracing():
    m = Matcher(CFG)
    out = m.tx(tx(0, 1, 100))                    # the dongle never spoke
    out += m.dongle_alive(5000 * MS)             # first dongle record comes later
    out += m.finish(9000 * MS)
    assert not evs(out, "lost")
    assert len(evs(out, "unobserved")) == 1


def test_dongle_silence_gap_breaks_coverage():
    m = Matcher(CFG)
    out = m.dongle_alive(0) + m.dongle_alive(1000 * MS)
    out += m.dongle_alive(10_000 * MS)           # 9 s of silence: trace was off
    out += m.tx(tx(0, 1, 2000))                  # inside the silence
    out += m.dongle_alive(11_000 * MS)
    assert not evs(out, "lost") and len(evs(out, "unobserved")) == 1


def test_duplicate_rx_is_a_dup_not_a_second_match():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 3, 100)) + m.rx(rx(0, 3, 100)) + m.rx(rx(0, 3, 100, lat_ms=5, flags=2))
    assert len(evs(out, "matched")) == 1
    assert len(evs(out, "dup")) == 1


def test_tx_resent_with_same_seq_is_one_logical_frame():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 3, 100)) + m.tx(tx(0, 3, 150)) + m.rx(rx(0, 3, 150))
    out += m.dongle_alive(3000 * MS)
    assert len(evs(out, "matched")) == 1 and not evs(out, "lost")


def test_reordered_rx_still_match():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 1, 100)) + m.tx(tx(0, 2, 110))
    out += m.rx(rx(0, 2, 110)) + m.rx(rx(0, 1, 100, lat_ms=12))
    assert len(evs(out, "matched")) == 2


def test_noseq_frames_are_counted_but_never_matched_nor_lost():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 0, 100, flags=2, kind=5))
    out += m.dongle_alive(5000 * MS)
    assert len(evs(out, "noseq")) == 1
    assert not evs(out, "lost") and not evs(out, "matched")


def test_sleeping_half_accrues_no_loss_and_no_gap():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 1, 100)) + m.rx(rx(0, 1, 100))
    m.half_sleep(0, 200 * MS)
    out += alive_until(m, 500, 70_000)           # 70 s asleep, the dongle keeps tracing
    m.half_wake(0, 70_000 * MS)
    out += m.tx(tx(0, 2, 70_100)) + m.rx(rx(0, 2, 70_100))
    out += m.dongle_alive(72_000 * MS)
    assert not evs(out, "lost") and not evs(out, "gap")
    assert len(evs(out, "matched")) == 2


def test_gap_event_for_a_run_of_lost_frames_between_two_received():
    m = Matcher(MatcherConfig(window_ms=1000, gap_ms=100))
    m.dongle_alive(0)
    out = m.tx(tx(0, 1, 100)) + m.rx(rx(0, 1, 100))
    for k, t in ((2, 150), (3, 200), (4, 250)):
        out += m.tx(tx(0, k, t))
    out += m.tx(tx(0, 5, 400)) + m.rx(rx(0, 5, 400))
    g = evs(out, "gap")
    assert len(g) == 1 and g[0]["missing"] == 3 and abs(g[0]["dur_ms"] - 250) < 1


def test_each_emitter_has_its_own_sequence_space():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 1, 100)) + m.tx(tx(1, 1, 101)) + m.rx(rx(1, 1, 101)) + m.rx(rx(0, 1, 100))
    ma = evs(out, "matched")
    assert sorted(e["e"] for e in ma) == [0, 1]


def test_tx_aimed_at_another_channel_is_not_a_loss():
    m = Matcher(CFG)
    m.dongle_alive(0)
    m.dongle_channel(82)
    out = m.tx(tx(1, 1, 100, ch=0x4F))
    out += m.dongle_alive(3000 * MS)
    assert not evs(out, "lost") and len(evs(out, "elsewhere")) == 1


def test_orphan_rx_reported_when_no_tx_ever_comes():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.rx(rx(0, 9, 100)) + m.dongle_alive(3000 * MS)
    assert len(evs(out, "orphan")) == 1


def test_late_rx_after_loss_recovers_the_frame():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.tx(tx(0, 1, 100)) + m.dongle_alive(1500 * MS)
    assert len(evs(out, "lost")) == 1
    out = m.rx(rx(0, 1, 100, lat_ms=1600))
    assert len(evs(out, "recovered")) == 1


def test_two_rx_ahead_of_their_tx_line_are_one_match_and_one_dup():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.rx(rx(0, 9, 100)) + m.rx(rx(0, 9, 100, lat_ms=8, flags=2)) + m.tx(tx(0, 9, 100))
    assert len(evs(out, "matched")) == 1 and len(evs(out, "dup")) == 1
    assert not evs(m.finish(10_000 * MS), "orphan")


def test_rx_of_seqless_kinds_is_not_an_orphan():
    m = Matcher(CFG)
    m.dongle_alive(0)
    out = m.rx(rx(0, 0, 100, kind=5)) + m.dongle_alive(3000 * MS)
    assert len(evs(out, "noseq_rx")) == 1 and not evs(out, "orphan")


def test_rx_flagged_noseq_is_never_an_orphan_even_for_an_unknown_kind():
    # The dongle says it (RXF_NOSEQ, format v2): no learning needed, no orphan.
    m = Matcher(CFG)
    out = m.rx(rx(0, 0, 100, kind=12, flags=0x04))
    out += alive_until(m, 100, 3000)
    assert len(evs(out, "noseq_rx")) == 1 and not evs(out, "orphan")
