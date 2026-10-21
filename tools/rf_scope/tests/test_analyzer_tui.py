"""Analyzer wiring (unwrap, sleep, reboot, ctl) and the TUI renderer."""

import pathlib

import pytest

import fake_run
from rftrace import jsonl, tui
from rftrace.analyzer import Analyzer
from rftrace.codec import ST_SLEEPING, ST_WOKE

NS = 1_000_000_000
H0 = 10 * NS


def periodic(src, host_s, t_us, up, source, state=0, **kw):
    d = {"type": "rec", "src": src, "port": "/dev/x", "host_ns": int(H0 + host_s * NS), "kind": "periodic",
         "t_us": t_us % (1 << 32), "uptime_s": up, "source": source, "state": state, "batt_dV": 40,
         "channel": 82, "frames": 0, "fails": 0, "dups": 0, "drops": 0}
    d.update(kw)
    return d


@pytest.fixture(scope="module")
def sample(tmp_path_factory):
    p = tmp_path_factory.mktemp("run") / "run.jsonl"
    fake_run.generate(p, seed=2, seconds=100, wrap_at_s=30)
    return p


def analyze(path):
    an = Analyzer(expect=(0, 1))
    for it in jsonl.read(path):
        an.feed(it)
    an.finish()
    return an


def test_wrap_of_the_left_clock_does_not_break_the_fit(sample):
    an = analyze(sample)
    assert -35e-6 < an.fits[0].slope - 1 < -12e-6          # generator: half is +25 ppm fast
    assert 5e-6 < an.fits[1].slope - 1 < 30e-6              # right: -18 ppm slow
    assert an.fits[0].jitter_us < 2500


def test_outage_on_the_right_is_reported_as_an_outage(sample):
    an = analyze(sample)
    gaps = [e for e in an.events if e["kind"] == "gap" and e["e"] == 1]
    assert gaps and any("outage" in g["text"] for g in gaps)
    w = an.stats.window(1, 0, 200, an.fits_for(1))
    assert w.gap_max_ms >= 250


def test_sleep_time_is_accounted_and_no_loss_is_attributed_to_it(sample):
    an = analyze(sample)
    for e in (0, 1):
        w = an.stats.window(e, 0, 200)
        assert 20 < w.asleep_s < 80


def test_reboot_of_a_half_is_detected_and_its_fit_reset():
    an = Analyzer()
    an.feed(periodic("left", 0, 100 * 1_000_000, 100, 0))
    an.feed(periodic("left", 1, 101 * 1_000_000, 101, 0))
    an.fits[0] = object()
    an.feed(periodic("left", 2, 2 * 1_000_000, 2, 0))
    assert an.reboots == 1 and 0 not in an.fits
    assert any(e["kind"] == "reboot" for e in an.events)


def test_ctl_events_drive_the_trace_status_and_giveup_is_surfaced():
    an = Analyzer()
    an.feed({"type": "ctl", "src": "left", "host_ns": H0, "event": "start_sent"})
    assert an.trace["left"] == "starting"
    an.feed({"type": "ctl", "src": "left", "host_ns": H0 + 1, "event": "giveup", "message": "left: press a key on the half to wake it"})
    assert an.trace["left"] == "gave_up"
    assert any("press a key" in e["text"] for e in an.events)


def test_sleeping_then_woke_records_make_an_asleep_interval():
    an = Analyzer()
    an.feed(periodic("left", 0, 5_000_000, 5, 0))
    an.feed(periodic("left", 1, 6_000_000, 6, 0, state=ST_SLEEPING))
    an.feed(periodic("left", 31, 3_606_000_000 % (1 << 32), 3606, 0, state=ST_WOKE))
    an.finish()
    assert abs(an.stats.window(0, 0, 100).asleep_s - 30.0) < 1e-6


def test_v3_dongle_health_is_summed_and_bench_knobs_are_surfaced():
    """Format v3: the dongle's periodic record carries its RX health under the
    per-radio settle (FIFO width flushes, the engine's longest gap, the
    settle wait) and every end says when an nRF24 knob is off its shipped
    value — the report shows both; a run without them says nothing new."""
    from rftrace import report
    an = Analyzer()
    an.feed(periodic("dongle", 0, 1_000_000, 1, 2, ovf=2, gap_max_ms=11, settle_ms=5))
    an.feed(periodic("dongle", 1, 2_000_000, 2, 2, ovf=1, gap_max_ms=37, settle_ms=7))
    an.feed(periodic("left", 1, 2_000_000, 2, 0, bench=0x01))
    an.feed(periodic("left", 2, 3_000_000, 3, 0, bench=0x01))
    h = an.dongle_health
    assert (h["ovf"], h["gap_max_ms"], h["settle_ms"], h["periodic"]) == (3, 37, 12, 2)
    benches = [e for e in an.events if e["kind"] == "bench"]
    assert len(benches) == 1 and "LEFT" in benches[0]["text"] and "feature" in benches[0]["text"]
    an.finish()
    txt = report.render(an)
    assert "FIFO width flushes 3" in txt and "engine gap max 37 ms" in txt and "settle wait 12 ms" in txt
    assert "BENCH" in txt


def test_v2_style_records_without_the_v3_fields_still_analyze():
    an = Analyzer()
    an.feed(periodic("dongle", 0, 1_000_000, 1, 2))
    assert an.dongle_health["ovf"] == 0 and not [e for e in an.events if e["kind"] == "bench"]


# ---- renderer ----------------------------------------------------------------

def test_render_wide_has_both_halves_dongle_and_status(sample):
    an = analyze(sample)
    txt = tui.plain(tui.render(an, an.last_ns, 110, 34, {"mode": "REPLAY", "run_file": "r.jsonl"}))
    assert "LEFT" in txt and "RIGHT" in txt and "DONGLE" in txt
    assert "lat p50" in txt and "loss" in txt
    assert any(c in txt for c in tui.SPARK[1:])
    assert txt.splitlines()[-1].startswith(" REPLAY")
    assert len(txt.splitlines()) <= 34


@pytest.mark.parametrize("w,h", [(80, 24), (60, 12), (45, 8), (40, 6), (200, 60)])
def test_render_fits_any_reasonable_terminal(sample, w, h):
    an = analyze(sample)
    lines = tui.render(an, an.last_ns, w, h, {})
    assert len(lines) <= h
    for ln in lines:
        assert sum(len(t) for t, _ in ln) <= w


def test_render_degrades_below_the_minimum():
    out = tui.plain(tui.render(Analyzer(), 0, 30, 4, {}))
    assert "too small" in out


def test_render_on_an_empty_run_does_not_crash():
    out = tui.plain(tui.render(Analyzer(expect=(0, 1)), 0, 100, 30, {}))
    assert "no data" in out


def test_pause_freezes_the_event_list(sample):
    an = analyze(sample)
    frozen = list(an.events)[:3]
    info = {"paused": True, "frozen_events": frozen}
    txt = tui.plain(tui.render(an, an.last_ns, 100, 30, info))
    assert frozen[0]["text"] in txt
    assert list(an.events)[-1]["text"] not in txt or frozen[-1] is list(an.events)[-1]


def test_reset_narrows_the_window(sample):
    an = analyze(sample)
    full = tui.View(an, 0, an.last_ns, None)
    cut = tui.View(an, 0, an.last_ns, an.last_ns - 5 * NS)
    assert cut.span <= 6 and full.span == 60


def test_sparkline_scales_and_compresses():
    s = tui.sparkline([0.0, 5.0, None, 10.0], 4)
    assert len(s) == 4 and s[2] == "·" and s[3] == "█"
    assert len(tui.sparkline([1.0] * 60, 20)) == 20


class FakeScr:
    def __init__(self, keys):
        self.keys, self.rows = list(keys), []

    def nodelay(self, v): pass
    def keypad(self, v): pass
    def getmaxyx(self): return (24, 100)
    def erase(self): self.rows = []
    def refresh(self): pass
    def addstr(self, y, x, text, attr=0): self.rows.append((y, x, text))
    def getch(self): return self.keys.pop(0) if self.keys else ord("q")


def test_curses_loop_draws_handles_keys_and_quits(sample, monkeypatch):
    import curses
    monkeypatch.setattr(curses, "curs_set", lambda n: None)
    monkeypatch.setattr(curses, "has_colors", lambda: False)
    monkeypatch.setattr(tui.time, "sleep", lambda s: None)
    an = analyze(sample)
    scr = FakeScr([ord("p"), ord("r"), ord("p")])
    info = {"mode": "T"}
    steps = []

    def step():
        steps.append(1)
        return an.last_ns, True

    tui.run(scr, an, step, info)
    assert len(steps) == 4 and info["reset_ns"] == an.last_ns and not info["paused"]
    assert any("LEFT" in t for _, _, t in scr.rows)
