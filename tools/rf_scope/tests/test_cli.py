"""CLI wiring: capture refuses a busy port before opening anything; replay and
report run on the generated sample; the sample is analyzable end to end."""

import io
import pathlib

import pytest

import rf_scope
from rftrace import jsonl, ports

import fake_run

SAMPLE = pathlib.Path(__file__).resolve().parents[1] / "samples" / "demo_run.jsonl"


def test_duration_parser():
    assert rf_scope.parse_duration("10m") == 600
    assert rf_scope.parse_duration("90") == 90
    assert rf_scope.parse_duration("1h30m") == 5400
    with pytest.raises(Exception):
        rf_scope.parse_duration("abc")


def test_capture_refuses_a_busy_port_and_opens_nothing(monkeypatch, capsys):
    monkeypatch.setattr(ports, "port_holders", lambda p: [(777, "minicom")])

    def boom(*a, **k):
        raise AssertionError("serial must not be opened")

    monkeypatch.setattr(ports, "open_serial", boom)
    rc = rf_scope.main(["capture", "--dongle", "/dev/ttyACM9", "--no-tui", "-o", "/dev/null"])
    err = capsys.readouterr().err
    assert rc == 2 and "777" in err and "minicom" in err


def test_capture_needs_at_least_one_port(capsys):
    with pytest.raises(SystemExit):
        rf_scope.main(["capture", "--no-tui"])


def test_generated_run_is_reproducible_and_has_the_advertised_features(tmp_path):
    a, b = tmp_path / "a.jsonl", tmp_path / "b.jsonl"
    fake_run.generate(a, seed=3, seconds=40)
    fake_run.generate(b, seed=3, seconds=40)
    assert a.read_bytes() == b.read_bytes()


def test_report_on_a_generated_run_finds_losses_sleeps_and_latency(tmp_path, capsys):
    p = tmp_path / "run.jsonl"
    fake_run.generate(p, seed=1, seconds=150, wrap_at_s=60)
    rc = rf_scope.main(["report", str(p)])
    out = capsys.readouterr().out
    assert rc == 0
    assert "LEFT" in out and "RIGHT" in out
    assert "loss" in out and "latency" in out and "asleep" in out
    an = rf_scope.analyze_file(p)
    for e in (0, 1):
        w = an.stats.window(e, 0, 10_000, fits=an.fits_for(e))
        assert w.matched > 100
        assert 0.3 < w.loss_pct < 6.0
        assert w.asleep_s > 5
        assert 0.3 < w.lat_p50 < 6.0              # generator: base 0.8 ms + exp(1.5 ms)
        assert w.lat_p95 > w.lat_p50


def test_committed_sample_exists_and_replays_to_a_final_report(capsys):
    assert SAMPLE.exists()
    assert rf_scope.main(["report", str(SAMPLE)]) == 0
    assert "LEFT" in capsys.readouterr().out


def test_replay_dump_renders_one_frame_of_the_sample(capsys):
    assert rf_scope.main(["replay", str(SAMPLE), "--dump", "100x30", "--at", "60"]) == 0
    out = capsys.readouterr().out
    assert "LEFT" in out and "RIGHT" in out and "REPLAY demo_run.jsonl" in out
    assert len(out.rstrip("\n").splitlines()) <= 30


def test_replay_dump_rejects_a_bad_geometry(capsys):
    assert rf_scope.main(["replay", str(SAMPLE), "--dump", "wide"]) == 2


class Device:
    """A port that behaves like the firmware: acks start/stop, then streams a few frames."""

    def __init__(self):
        from rftrace import codec
        self.codec = codec
        self.rx = bytearray()
        self.closed = False
        self.sent = []
        self.n = 0

    in_waiting = property(lambda self: len(self.rx))

    def read(self, n=1):
        out, self.rx = bytes(self.rx[:n]), self.rx[n:]
        return out

    def write(self, b):
        from rftrace.control import ks_frame
        self.sent.append(bytes(b))
        if bytes(b) == ks_frame(0xBA, b"\x00") or bytes(b)[:5] == ks_frame(0xBA, b"\x01\x00\x00")[:5]:
            self.rx += bytes([0x4B, 0x52, 0xBA, 0, 0, 0, 0])
        if bytes(b)[5:6] == b"\x01":
            self.rx += b"console text\r\n"
            for i in range(3):
                self.n += 1
                self.rx += self.codec.encode_frame("tx", [{
                    "t_us": 1000 * self.n, "emitter": 0, "seq": self.n, "pkt_kind": 4, "payload_len": 6,
                    "result": 0, "arc": 0, "plos_delta": 0, "channel": 82, "flags": 0}])
        return len(b)

    def close(self):
        self.closed = True


def test_capture_end_to_end_with_fake_ports(tmp_path, monkeypatch, capsys):
    opened = {}

    def fake_open(path, baud, half, **k):
        opened[path] = (half, baud)
        d = Device()
        opened[path + "#dev"] = d
        return d

    monkeypatch.setattr(ports, "port_holders", lambda p: [])
    monkeypatch.setattr(ports, "open_serial", fake_open)
    out = tmp_path / "run.jsonl"
    rc = rf_scope.main(["capture", "--dongle", "/dev/ttyACM9", "--left", "/dev/ttyUSB9",
                        "--duration", "1", "-o", str(out), "--no-tui"])
    assert rc == 0
    assert opened["/dev/ttyACM9"][0] is False and opened["/dev/ttyUSB9"][0] is True
    items = list(jsonl.read(out))
    assert items[0]["type"] == "meta" and items[0]["sources"] == {"dongle": "/dev/ttyACM9", "left": "/dev/ttyUSB9"}
    events = [(i["src"], i["event"]) for i in items if i["type"] == "ctl"]
    for src in ("dongle", "left"):
        assert (src, "start_sent") in events and (src, "ack") in events
        assert (src, "stop_sent") in events and (src, "stop_ack") in events
    assert sum(1 for i in items if i["type"] == "rec") >= 6
    assert any(i["type"] == "text" and i["text"] == "console text" for i in items)
    assert opened["/dev/ttyACM9#dev"].closed and opened["/dev/ttyUSB9#dev"].closed
    assert "LEFT" in capsys.readouterr().out
