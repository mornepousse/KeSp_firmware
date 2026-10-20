"""JSONL round trip, start/stop repeat logic with a fake port, port safety."""

import json
import os

import pytest

from rftrace import codec, jsonl, ports
from rftrace.control import StartController, ks_frame
from rftrace.source import SourceRunner
from rftrace.stream import SourceStream, ACK


# ---- JSONL ------------------------------------------------------------------

def test_jsonl_round_trip_keeps_every_item_type(tmp_path):
    items = [
        {"type": "meta", "version": 1, "sources": {"dongle": "/dev/ttyACM0"}},
        {"type": "rec", "src": "left", "port": "/dev/ttyUSB0", "host_ns": 5, "kind": "tx",
         "t_us": 7, "emitter": 0, "seq": 255, "pkt_kind": 4, "payload_len": 6, "result": 1,
         "arc": 15, "plos_delta": 2, "channel": 82, "flags": 3},
        {"type": "text", "src": "left", "host_ns": 6, "text": "I (123) rftrace: ON — été"},
        {"type": "ctl", "src": "dongle", "host_ns": 9, "event": "ack", "attempts": 3},
    ]
    p = tmp_path / "run.jsonl"
    with jsonl.Writer(p) as w:
        for it in items:
            w.write(it)
    assert list(jsonl.read(p)) == items


def test_jsonl_ignores_a_truncated_last_line(tmp_path):
    p = tmp_path / "run.jsonl"
    p.write_text(json.dumps({"type": "ctl", "host_ns": 1, "src": "dongle", "event": "ack"}) + "\n" + '{"type": "re')
    assert len(list(jsonl.read(p))) == 1


def test_record_item_is_built_from_a_decoded_frame():
    fr = codec.decode_frame(codec.encode_frame("tx", [
        {"t_us": 1, "emitter": 0, "seq": 2, "pkt_kind": 4, "payload_len": 6, "result": 0,
         "arc": 0, "plos_delta": 0, "channel": 82, "flags": 0}]))
    items = jsonl.frame_items(fr, "left", "/dev/ttyUSB0", 42)
    assert items == [{"type": "rec", "src": "left", "port": "/dev/ttyUSB0", "host_ns": 42,
                      "kind": "tx", **fr["records"][0]}]


# ---- start / stop controller -------------------------------------------------

def test_ks_frame_matches_the_documented_example():
    f = ks_frame(0xBA, bytes([0x01]) + (120).to_bytes(2, "little"))
    assert f[:2] == b"KS" and f[2] == 0xBA and f[3:5] == b"\x03\x00"
    assert f[-1] == codec.crc8(f[5:-1])


def test_start_is_repeated_every_250ms_until_acked():
    c = StartController(duration_s=600, interval_s=0.25, give_up_s=30)
    sent = []
    for k in range(0, 13):                       # 0 .. 1.2 s in 100 ms steps
        r = c.poll(k * 0.1)
        if r:
            sent.append((k * 0.1, r))
    assert [round(t, 1) for t, _ in sent] == [0.0, 0.3, 0.6, 0.9, 1.2]
    assert all(r == ks_frame(0xBA, b"\x01" + (600).to_bytes(2, "little")) for _, r in sent)
    c.on_ack(1.3)
    assert c.state == "on" and c.attempts == 5
    assert c.poll(5.0) is None


def test_data_frame_counts_as_an_ack():
    c = StartController(600)
    c.poll(0)
    c.on_data(0.4)
    assert c.state == "on" and c.poll(1.0) is None


def test_gives_up_with_a_message_about_waking_the_half():
    c = StartController(600, give_up_s=5)
    for k in range(0, 80):
        c.poll(k * 0.1)
    assert c.state == "gave_up"
    assert "press a key" in c.message("left")
    assert c.poll(9.0) is None


def test_duration_is_clamped_to_u16():
    c = StartController(10 ** 6)
    assert c.poll(0)[6:8] == (65535).to_bytes(2, "little")


def test_stop_repeats_until_acked_then_ends_quickly():
    c = StartController(600)
    c.poll(0)
    c.on_ack(0.1)
    c.request_stop(1.0)
    assert c.poll(1.0) == ks_frame(0xBA, b"\x00")
    assert c.poll(1.1) is None
    assert c.poll(1.25) == ks_frame(0xBA, b"\x00")
    c.on_ack(1.3)
    assert c.state == "off"


def test_stop_gives_up_after_its_budget():
    c = StartController(600, stop_budget_s=1.0)
    c.poll(0); c.on_ack(0.1)
    c.request_stop(1.0)
    for k in range(0, 40):
        c.poll(1.0 + k * 0.1)
    assert c.state == "off"


# ---- stream / runner with a fake port ---------------------------------------

class FakePort:
    def __init__(self):
        self.rx = bytearray()
        self.written = []
        self.closed = False

    @property
    def in_waiting(self):
        return len(self.rx)

    def read(self, n=1):
        out, self.rx = bytes(self.rx[:n]), self.rx[n:]
        return out

    def write(self, b):
        self.written.append(bytes(b))
        return len(b)

    def close(self):
        self.closed = True


class Clock:
    def __init__(self):
        self.t = 0.0

    def mono(self):
        return self.t

    def ns(self):
        return int(self.t * 1e9)


def tx_frame(seq):
    return codec.encode_frame("tx", [{"t_us": seq, "emitter": 0, "seq": seq, "pkt_kind": 4,
                                      "payload_len": 6, "result": 0, "arc": 0, "plos_delta": 0,
                                      "channel": 82, "flags": 0}])


def test_stream_finds_ack_frames_and_lines_across_chunks():
    s = SourceStream()
    got = []
    blob = b"I (10) boot: hello\r\n" + ACK + tx_frame(1) + b"HB up=3\n"
    for i in range(0, len(blob), 3):
        got += s.feed(blob[i:i + 3])
    kinds = [g[0] for g in got]
    assert kinds.count("ack") == 1 and kinds.count("frame") == 1
    assert [g[1] for g in got if g[0] == "line"] == ["I (10) boot: hello", "HB up=3"]


def test_runner_repeats_start_until_ack_then_streams_items():
    clock, port = Clock(), FakePort()
    items = []
    r = SourceRunner("left", "/dev/ttyUSB0", port, StartController(600), items.append,
                     clock_mono=clock.mono, clock_ns=clock.ns)
    for _ in range(4):                           # t = 0 .. 0.3
        r.pump()
        clock.t += 0.1
    assert len(port.written) == 2                # 0.0 and 0.3 (0.25 elapsed)
    port.rx += b"console noise\n" + ACK
    clock.t = 0.5
    r.pump()
    port.rx += tx_frame(7)
    clock.t = 0.6
    r.pump()
    n_before = len(port.written)
    clock.t = 3.0
    r.pump()
    assert len(port.written) == n_before        # acked: no more start
    ctl = [i for i in items if i["type"] == "ctl"]
    assert [c["event"] for c in ctl] == ["start_sent", "ack"]
    assert ctl[1]["attempts"] == 2
    recs = [i for i in items if i["type"] == "rec"]
    assert recs[0]["seq"] == 7 and recs[0]["host_ns"] == int(0.6 * 1e9) and recs[0]["src"] == "left"
    assert [i["text"] for i in items if i["type"] == "text"] == ["console noise"]


def test_runner_stop_on_exit_sends_stop_and_waits_for_ack():
    clock, port = Clock(), FakePort()
    items = []
    r = SourceRunner("dongle", "/dev/ttyACM0", port, StartController(600), items.append,
                     clock_mono=clock.mono, clock_ns=clock.ns)
    r.pump()
    port.rx += ACK
    clock.t = 0.1
    r.pump()
    r.request_stop()
    clock.t = 0.2
    r.pump()
    assert port.written[-1] == ks_frame(0xBA, b"\x00")
    port.rx += ACK
    clock.t = 0.3
    r.pump()
    assert r.done
    assert "stop_ack" in [i.get("event") for i in items]


def test_runner_reports_giveup():
    clock, port = Clock(), FakePort()
    items = []
    r = SourceRunner("left", "/dev/ttyUSB0", port, StartController(600, give_up_s=2), items.append,
                     clock_mono=clock.mono, clock_ns=clock.ns)
    for _ in range(40):
        r.pump()
        clock.t += 0.1
    ev = [i for i in items if i["type"] == "ctl" and i["event"] == "giveup"]
    assert len(ev) == 1 and "press a key" in ev[0]["message"]


# ---- port safety --------------------------------------------------------------

def test_busy_port_is_refused_with_the_holder_named():
    with pytest.raises(ports.PortBusy) as e:
        ports.check_port_free("/dev/ttyUSB0", holders_fn=lambda p: [(4242, "minicom")])
    assert "4242" in str(e.value) and "minicom" in str(e.value)


def test_free_port_passes():
    ports.check_port_free("/dev/ttyUSB0", holders_fn=lambda p: [])


def test_proc_scan_finds_a_holder_and_skips_ourselves(tmp_path):
    dev = tmp_path / "ttyUSB9"
    dev.write_text("")
    proc = tmp_path / "proc"
    for pid, comm, target in ((100, "picocom", dev), (os.getpid(), "me", dev), (200, "other", tmp_path / "x")):
        (proc / str(pid) / "fd").mkdir(parents=True)
        (proc / str(pid) / "fd" / "3").symlink_to(target)
        (proc / str(pid) / "comm").write_text(comm + "\n")
    assert ports.holders_from_proc(str(dev), proc_root=str(proc)) == [(100, "picocom")]


def test_fuser_output_is_parsed(monkeypatch):
    class R:
        returncode, stdout, stderr = 0, " 123  456", "/dev/x: "
    monkeypatch.setattr(ports.shutil, "which", lambda n: "/usr/bin/fuser")
    monkeypatch.setattr(ports.subprocess, "run", lambda *a, **k: R())
    monkeypatch.setattr(ports, "_comm_of", lambda pid: f"proc{pid}")
    assert ports.holders_from_fuser("/dev/x") == [(123, "proc123"), (456, "proc456")]


class RecSerial:
    log = []

    def __init__(self, *a, **k):
        assert not a and not k, "must be constructed unopened"
        self.is_open = False
        RecSerial.log = []

    def __setattr__(self, k, v):
        if k != "is_open" and not k.startswith("_"):
            RecSerial.log.append(("set", k, v, getattr(self, "is_open", False)))
        object.__setattr__(self, k, v)

    def open(self):
        RecSerial.log.append(("open",))
        object.__setattr__(self, "is_open", True)


def test_half_port_has_dtr_rts_low_before_open_and_never_touched_after():
    s = ports.open_serial("/dev/ttyUSB0", 115200, half=True, serial_factory=RecSerial)
    log = RecSerial.log
    i_open = log.index(("open",))
    before = {(k, v) for op, k, v, o in (e for e in log[:i_open])} if False else {(e[1], e[2]) for e in log[:i_open]}
    assert ("dtr", False) in before and ("rts", False) in before
    assert not any(e[0] == "set" and e[1] in ("dtr", "rts") for e in log[i_open + 1:])
    assert s.is_open


def test_dongle_port_keeps_pyserial_defaults_for_the_control_lines():
    ports.open_serial("/dev/ttyACM0", 115200, half=False, serial_factory=RecSerial)
    assert not any(e[0] == "set" and e[1] in ("dtr", "rts") for e in RecSerial.log)
