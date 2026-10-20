"""rf_scope stress: scenario -> RF_STRESS request bytes, the start/stop
controller of the stress run, the runner that starts it once the trace is on
and stops it before the trace, the CLI with fake ports, and the report's
"stress" section (offered vs delivered, loss, retries, latency under load,
ring drops, the dongle's stress counter, per-10-min windows for a soak)."""

import struct

import pytest

import rf_scope
from rftrace import codec, jsonl, ports, stress
from rftrace.analyzer import Analyzer
from rftrace.control import StartController, ks_frame
from rftrace.source import SourceRunner
from rftrace.stream import kr_reply

from test_io_ctl import Clock, FakePort


def ack(cmd, sub, session, remaining):
    return kr_reply(cmd, 0, bytes([sub]) + session.to_bytes(4, "little") + remaining.to_bytes(2, "little"))


# ---- scenario -> request ------------------------------------------------------

def test_scenarios_map_to_firmware_patterns():
    assert stress.scenario_params("burst", 30)["pattern"] == "burst"
    assert stress.scenario_params("max", 30)["pattern"] == "max"
    assert stress.scenario_params("held", 30)["pattern"] == "held"
    r = stress.scenario_params("rate", 30, rate=200)
    assert r["pattern"] == "rate" and r["rate_hz"] == 200
    s = stress.scenario_params("soak", None)
    assert s["pattern"] == "rate" and s["duration_s"] == 3600 and s["rate_hz"] == stress.DEFAULT_RATE["soak"]
    assert stress.scenario_params("burst", None)["duration_s"] == 30
    b = stress.scenario_params("burst", 30, burst_len=4, burst_pause_ms=50)
    assert (b["burst_len"], b["burst_pause_ms"]) == (4, 50)


def test_start_payload_is_the_13_bytes_the_firmware_parses():
    p = stress.scenario_params("rate", 90, rate=250, count=0x01020304)
    b = stress.encode_start(p)
    assert len(b) == 13
    assert b == struct.pack("<BBHIHBH", 1, 0, 250, 0x01020304, 90, p["burst_len"], p["burst_pause_ms"])
    assert stress.encode_start(stress.scenario_params("held", 30))[1] == 3
    assert stress.encode_start(stress.scenario_params("max", 30))[1] == 2
    assert stress.encode_start(stress.scenario_params("burst", 30))[1] == 1
    assert stress.encode_stop() == b"\x00"


def test_out_of_range_parameters_are_refused_before_anything_is_sent():
    with pytest.raises(ValueError):
        stress.scenario_params("rate", 30, rate=0)
    with pytest.raises(ValueError):
        stress.scenario_params("rate", 30, rate=2001)
    with pytest.raises(ValueError):
        stress.scenario_params("burst", 30, burst_len=0)
    with pytest.raises(ValueError):
        stress.scenario_params("max", 70000)
    with pytest.raises(ValueError):
        stress.scenario_params("nope", 30)


# ---- the stress controller ---------------------------------------------------

def test_stress_start_repeats_until_acked_and_keeps_the_session():
    p = stress.scenario_params("max", 30)
    c = stress.StressController(p)
    frames = [c.poll(t / 10) for t in range(0, 6)]
    sent = [f for f in frames if f]
    assert len(sent) == 2 and all(f == ks_frame(0xBB, stress.encode_start(p)) for f in sent)
    c.on_ack(0.6, {"sub": 1, "session": 9, "remaining_s": 30})
    assert c.state == "on" and c.session == 9 and c.remaining_s == 30
    assert c.poll(5.0) is None


def test_stress_refused_is_final():
    c = stress.StressController(stress.scenario_params("max", 30))
    c.poll(0)
    c.on_refused(0.1, 0x01)
    assert c.state == "refused" and c.poll(1.0) is None
    assert "status 1" in c.message("left")


def test_stress_never_started_needs_no_stop():
    c = stress.StressController(stress.scenario_params("max", 30))
    c.request_stop(0.0)
    assert c.state == "off" and c.poll(0.1) is None


def test_stress_stop_repeats_until_acked():
    c = stress.StressController(stress.scenario_params("max", 30))
    c.poll(0); c.on_ack(0.1, {"sub": 1, "session": 2, "remaining_s": 30})
    c.request_stop(1.0)
    assert c.poll(1.0) == ks_frame(0xBB, b"\x00")
    assert c.poll(1.1) is None and c.poll(1.3) == ks_frame(0xBB, b"\x00")
    c.on_ack(1.4, {"sub": 0, "session": 2, "remaining_s": 0})
    assert c.state == "off"


# ---- the runner: trace first, then stress; stress stopped before the trace -----

def test_runner_starts_stress_only_after_the_trace_and_stops_it_first():
    clock, port = Clock(), FakePort()
    items = []
    p = stress.scenario_params("burst", 30)
    r = SourceRunner("left", "/dev/ttyUSB0", port, StartController(60), items.append,
                     clock_mono=clock.mono, clock_ns=clock.ns, stress=stress.StressController(p))
    r.pump()
    assert all(w[2] == 0xBA for w in port.written), "no stress request before the trace is on"
    port.rx += ack(0xBA, 1, 5, 60)
    clock.t = 0.1
    r.pump()
    assert port.written[-1] == ks_frame(0xBB, stress.encode_start(p)), "stress start once the trace is on"
    port.rx += ack(0xBB, 1, 3, 30)
    clock.t = 0.2
    r.pump()
    assert r.stress.state == "on"
    # stop: the stress first, the trace only once the stress is off
    r.request_stop()
    clock.t = 0.3
    r.pump()
    assert port.written[-1] == ks_frame(0xBB, b"\x00")
    r.request_stop()
    clock.t = 0.4
    r.pump()
    assert all(w != ks_frame(0xBA, b"\x00") for w in port.written), "trace still on while the stress stops"
    port.rx += ack(0xBB, 0, 3, 0)
    clock.t = 0.5
    r.pump()
    r.request_stop()
    clock.t = 0.6
    r.pump()
    assert port.written[-1] == ks_frame(0xBA, b"\x00")
    port.rx += ack(0xBA, 0, 5, 0)
    clock.t = 0.7
    r.pump()
    assert r.done
    ev = [i["event"] for i in items if i["type"] == "ctl"]
    assert ev == ["start_sent", "ack", "stress_start_sent", "stress_ack", "stress_stop_sent",
                  "stress_stop_ack", "stop_sent", "stop_ack"]
    st = [i for i in items if i.get("event") == "stress_start_sent"][0]
    assert st["params"]["pattern"] == "burst" and st["scenario"] == "burst"
    sa = [i for i in items if i.get("event") == "stress_ack"][0]
    assert sa["session"] == 3 and sa["remaining_s"] == 30


def test_runner_reports_a_refused_stress_and_still_stops_the_trace():
    clock, port = Clock(), FakePort()
    items = []
    r = SourceRunner("right", "/dev/ttyUSB1", port, StartController(60), items.append,
                     clock_mono=clock.mono, clock_ns=clock.ns,
                     stress=stress.StressController(stress.scenario_params("max", 30)))
    r.pump()
    port.rx += ack(0xBA, 1, 1, 60)
    clock.t = 0.1
    r.pump()
    port.rx += kr_reply(0xBB, 0x01)
    clock.t = 0.2
    r.pump()
    assert [i for i in items if i.get("event") == "stress_refused"][0]["status"] == 1
    r.request_stop()
    clock.t = 0.3
    r.pump()
    assert port.written[-1] == ks_frame(0xBA, b"\x00")


# ---- CLI with fake ports ---------------------------------------------------------

class FakeHalf:
    """A half: acks RF_TRACE and RF_STRESS, sends a stress TX record per pump while stressed."""

    def __init__(self, emitter=0):
        self.rx = bytearray()
        self.written = []
        self.closed = False
        self.e = emitter
        self.stressing = False
        self.seq = 0
        self.t = 0

    @property
    def in_waiting(self):
        if self.stressing:
            self.seq = (self.seq + 1) & 0xFF
            self.t += 10_000
            self.rx += codec.encode_frame("tx", [{"t_us": self.t, "emitter": self.e, "seq": self.seq,
                                                  "pkt_kind": 0xA, "payload_len": 6, "result": 0, "arc": 1,
                                                  "plos_delta": 0, "channel": 82, "flags": 0}])
        return len(self.rx)

    def read(self, n=1):
        out, self.rx = bytes(self.rx[:n]), self.rx[n:]
        return out

    def write(self, b):
        b = bytes(b)
        self.written.append(b)
        cmd, sub = b[2], b[5]
        if cmd == 0xBB:
            self.stressing = sub == 1
        self.rx += ack(cmd, sub, 7, 30 if sub == 1 else 0)
        return len(b)

    def close(self):
        self.closed = True


class FakeDongle(FakeHalf):
    def write(self, b):
        b = bytes(b)
        self.written.append(b)
        self.rx += ack(0xBA, b[5], 4, 60 if b[5] == 1 else 0)
        return len(b)

    @property
    def in_waiting(self):
        return len(self.rx)


def test_stress_cli_refuses_a_busy_port_and_opens_nothing(monkeypatch, capsys):
    monkeypatch.setattr(ports, "port_holders", lambda p: [(31, "kesp_cdc")] if "ACM" in p else [])

    def boom(*a, **k):
        raise AssertionError("serial must not be opened")

    monkeypatch.setattr(ports, "open_serial", boom)
    rc = rf_scope.main(["stress", "--dongle", "/dev/ttyACM9", "--left", "/dev/ttyUSB9",
                        "--scenario", "max", "--no-tui", "-o", "/dev/null"])
    assert rc == 2 and "kesp_cdc" in capsys.readouterr().err


def test_stress_cli_needs_the_dongle_and_a_half(capsys):
    assert rf_scope.main(["stress", "--left", "/dev/ttyUSB9", "--scenario", "max", "--no-tui"]) == 2
    assert rf_scope.main(["stress", "--dongle", "/dev/ttyACM9", "--scenario", "max", "--no-tui"]) == 2


def test_stress_cli_bad_rate_is_refused_before_any_port(monkeypatch, capsys):
    monkeypatch.setattr(ports, "port_holders", lambda p: (_ for _ in ()).throw(AssertionError("no port check")))
    assert rf_scope.main(["stress", "--dongle", "/dev/ttyACM9", "--left", "/dev/ttyUSB9",
                          "--scenario", "rate", "--rate", "0", "--no-tui"]) == 2


def test_stress_cli_end_to_end_with_fake_ports(tmp_path, monkeypatch, capsys):
    opened = {}

    def fake_open(path, baud, half, **k):
        d = FakeHalf() if half else FakeDongle()
        opened[path] = (half, d)
        return d

    monkeypatch.setattr(ports, "port_holders", lambda p: [])
    monkeypatch.setattr(ports, "open_serial", fake_open)
    out = tmp_path / "stress.jsonl"
    rc = rf_scope.main(["stress", "--dongle", "/dev/ttyACM9", "--left", "/dev/ttyUSB9", "--scenario", "burst",
                        "--duration", "1", "-o", str(out), "--no-tui"])
    assert rc == 0
    assert opened["/dev/ttyUSB9"][0] is True and opened["/dev/ttyACM9"][0] is False
    half = opened["/dev/ttyUSB9"][1]
    assert not half.stressing, "the stress was stopped"
    assert any(w[2] == 0xBB and w[5] == 1 for w in half.written)
    assert not any(w[2] == 0xBB for w in opened["/dev/ttyACM9"][1].written), "never a stress request to the dongle"
    items = list(jsonl.read(out))
    assert items[0]["type"] == "meta" and items[0]["scenario"]["name"] == "burst"
    ev = [(i["src"], i["event"]) for i in items if i["type"] == "ctl"]
    for e in ("stress_start_sent", "stress_ack", "stress_stop_sent", "stress_stop_ack"):
        assert ("left", e) in ev
    assert ("left", "stop_ack") in ev and ("dongle", "stop_ack") in ev
    assert ev.index(("left", "stress_stop_ack")) < ev.index(("left", "stop_sent")), "stress off before the trace"
    assert sum(1 for i in items if i["type"] == "rec" and i["pkt_kind"] == 0xA) > 3
    report = capsys.readouterr().out
    assert "STRESS" in report and "burst" in report
    assert all(p[1].closed for p in opened.values())


# ---- report: the stress section ---------------------------------------------------

def stress_run(path, seconds=30, rate=100, scenario="rate", lose_every=50, maxrt_every=25,
               dongle_drops=2, soak=False):
    """A stress run file: the left at `rate` Hz, the dongle receiving."""
    NS = 1_000_000_000
    H0 = 7_000_000_000_000
    OFF = 3_000_000_000                  # dongle clock = half clock + OFF
    items = [{"type": "meta", "version": 1, "tool": "test", "sources": {"dongle": "d", "left": "l"},
              "scenario": {"name": scenario, "params": stress.scenario_params(scenario, seconds, rate=rate)}}]
    put = items.append
    put({"type": "ctl", "src": "dongle", "port": "d", "host_ns": H0, "event": "ack", "session": 1, "remaining_s": 60})
    put({"type": "ctl", "src": "left", "port": "l", "host_ns": H0, "event": "ack", "session": 1, "remaining_s": 60})
    put({"type": "ctl", "src": "left", "port": "l", "host_ns": H0 + 1000, "event": "stress_ack",
         "session": 4, "remaining_s": seconds})
    n = seconds * rate
    acked = 0
    for k in range(n):
        tau_us = 1_000_000 + k * (1_000_000 // rate)
        res = 1 if k % maxrt_every == maxrt_every - 1 else 0
        acked += res == 0
        host = H0 + tau_us * 1000 + 16_000_000
        put({"type": "rec", "src": "left", "port": "l", "host_ns": host, "kind": "tx", "t_us": tau_us % (1 << 32),
             "emitter": 0, "seq": k & 0xFF, "pkt_kind": 0xA, "payload_len": 6, "result": res,
             "arc": 15 if res else k % 3, "plos_delta": 1 if res else 0, "channel": 82, "flags": 0})
        if k % lose_every != lose_every - 1:
            delay = 800 + (k % 7) * 300
            put({"type": "rec", "src": "dongle", "port": "d", "host_ns": H0 + (tau_us + delay) * 1000 + 1_000_000,
                 "kind": "rx", "t_us": (tau_us + delay + OFF) % (1 << 32), "emitter": 0, "seq": k & 0xFF,
                 "pkt_kind": 0xA, "payload_len": 6, "pipe": 0, "rpd": 1, "channel": 82, "fifo_depth": 1, "flags": 0})
    for sec in range(seconds + 3):
        put({"type": "rec", "src": "dongle", "port": "d", "host_ns": H0 + sec * NS + 500_000_000, "kind": "periodic",
             "t_us": (sec * 1_000_000 + 500_000 + OFF) % (1 << 32), "uptime_s": 3000 + sec, "source": 2, "state": 0,
             "batt_dV": 0, "channel": 82, "frames": rate, "fails": 0, "dups": 0,
             "drops": dongle_drops if sec == 5 else 0, "session": 1, "stress": rate if 1 <= sec <= seconds else 0})
        put({"type": "rec", "src": "left", "port": "l", "host_ns": H0 + sec * NS + 300_000_000, "kind": "periodic",
             "t_us": sec * 1_000_000 + 300_000, "uptime_s": sec, "source": 0, "state": 0,
             "batt_dV": 40, "channel": 82, "frames": rate, "fails": 0, "dups": 0, "drops": 0, "session": 1,
             "stress": rate if 1 <= sec <= seconds else 0})
    end = H0 + (seconds + 3) * NS
    put({"type": "ctl", "src": "left", "port": "l", "host_ns": end, "event": "stress_stop_ack", "session": 4})
    items[1:] = sorted(items[1:], key=lambda i: i["host_ns"])
    with jsonl.Writer(path) as w:
        for it in items:
            w.write(it)
    return n, acked


def test_report_stress_section_offered_delivered_loss_latency(tmp_path, capsys):
    p = tmp_path / "s.jsonl"
    n, acked = stress_run(p, seconds=30, rate=100, lose_every=50)
    assert rf_scope.main(["report", str(p)]) == 0
    out = capsys.readouterr().out
    sec = out[out.index("STRESS"):]
    assert "scenario rate" in sec and "requested 100.0/s" in sec
    assert f"offered {n} frames" in sec
    assert "loss 2.00 %" in sec                        # one in 50
    assert f"MAX_RT {n // 25}" in sec
    assert "latency under load" in sec and "p95" in sec
    assert "dongle dropped (stress counter) 3000" in sec
    assert f"half acked {acked}" in sec
    assert "trace ring drops: half 0, dongle 2" in sec
    assert "transitions_ecrasees" in sec               # not in the trace: says where to read it


def test_stress_analyzer_ignores_real_frames():
    an = Analyzer(only_pkt_kind=0xA)
    real = {"type": "rec", "src": "left", "port": "l", "host_ns": 1, "kind": "tx", "t_us": 1, "emitter": 0,
            "seq": 1, "pkt_kind": 4, "payload_len": 6, "result": 0, "arc": 0, "plos_delta": 0, "channel": 82,
            "flags": 0}
    an.feed(real)
    an.feed(dict(real, pkt_kind=0xA, host_ns=2))
    an.finish()
    assert an.stats.window(0, 0, 10).frames == 1


def test_soak_report_has_10_minute_windows(tmp_path, capsys):
    p = tmp_path / "soak.jsonl"
    stress_run(p, seconds=1500, rate=2, scenario="soak", lose_every=40)
    assert rf_scope.main(["report", str(p)]) == 0
    out = capsys.readouterr().out
    sec = out[out.index("STRESS"):]
    assert "per 600 s window" in sec
    rows = [ln for ln in sec.splitlines() if ln.strip().startswith(("00:", "10:", "20:"))]
    assert len(rows) == 3, sec


def test_a_run_without_stress_has_no_stress_section(capsys):
    from test_cli import SAMPLE
    assert rf_scope.main(["report", str(SAMPLE)]) == 0
    assert "STRESS" not in capsys.readouterr().out
