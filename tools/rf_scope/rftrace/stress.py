"""rf-scope stress scenarios: what to ask a half's generator (KS_CMD_RF_STRESS,
0xBB, main/comm/rf/rf_stress.h) and how to start and stop it.

    stop : [0x00]
    start: [0x01][pattern u8][rate_hz u16][count u32][duration_s u16]
           [burst_len u8][burst_pause_ms u16]                     13 bytes, LE

The generator runs on the half and ends by itself (count, duration); the tool
starts it once the half's trace is on (so every stress frame has its TX
record), stops it at the end (Ctrl-C included) BEFORE stopping the trace, and
reads the result like any capture. Stress frames are of their own kind
(PKT_TYPE_STRESS, 0xA): the dongle drops them before the engine — nothing is
ever typed on the host.

Pure: no I/O; time is passed in.
"""

from __future__ import annotations

import struct

from .control import StartController, ks_frame

CMD_RF_STRESS = 0xBB
PATTERNS = {"rate": 0, "burst": 1, "max": 2, "held": 3}
SCENARIOS = ("burst", "max", "held", "rate", "soak")
PATTERN_OF = {"burst": "burst", "max": "max", "held": "held", "rate": "rate", "soak": "rate"}
DEFAULT_DURATION_S = {"soak": 3600}
DEFAULT_DURATION = 30
DEFAULT_RATE = {"rate": 50, "soak": 20}
HELD_HZ = 10                    # RF_STRESS_HELD_PERIOD_MS = 100
RATE_MAX_HZ = 2000              # RF_STRESS_RATE_MAX_HZ
SOAK_WINDOW_S = 600
PKT_STRESS = 0xA


def scenario_params(scenario: str, duration_s: int | None, rate: int | None = None, burst_len: int = 10,
                    burst_pause_ms: int = 200, count: int = 0) -> dict:
    """The generator's parameters for a scenario. Raises ValueError on what the
    firmware would refuse (or could not encode), before anything is sent."""
    if scenario not in PATTERN_OF:
        raise ValueError(f"unknown scenario {scenario!r} (one of {', '.join(SCENARIOS)})")
    pattern = PATTERN_OF[scenario]
    dur = DEFAULT_DURATION_S.get(scenario, DEFAULT_DURATION) if duration_s is None else int(duration_s)
    if not 1 <= dur <= 65535:
        raise ValueError(f"duration {dur} s out of 1..65535")
    r = int(rate if rate is not None else DEFAULT_RATE.get(scenario, 0))
    if pattern == "rate" and not 1 <= r <= RATE_MAX_HZ:
        raise ValueError(f"rate {r} Hz out of 1..{RATE_MAX_HZ}")
    if not 0 <= r <= 65535:
        raise ValueError(f"rate {r} Hz out of range")
    if pattern == "burst" and not 1 <= burst_len <= 255:
        raise ValueError(f"burst length {burst_len} out of 1..255")
    if not 0 <= burst_len <= 255 or not 0 <= burst_pause_ms <= 65535 or not 0 <= count <= 0xFFFFFFFF:
        raise ValueError("burst length / pause / count out of range")
    return {"scenario": scenario, "pattern": pattern, "rate_hz": r, "count": int(count), "duration_s": dur,
            "burst_len": int(burst_len), "burst_pause_ms": int(burst_pause_ms)}


def encode_start(p: dict) -> bytes:
    return struct.pack("<BBHIHBH", 0x01, PATTERNS[p["pattern"]], p["rate_hz"], p["count"], p["duration_s"],
                       p["burst_len"], p["burst_pause_ms"])


def encode_stop() -> bytes:
    return b"\x00"


def requested_rate(p: dict) -> float | None:
    """Frames per second asked for, when the pattern has one."""
    if p.get("pattern") == "rate":
        return float(p["rate_hz"])
    if p.get("pattern") == "held":
        return float(HELD_HZ)
    return None


class StressController(StartController):
    """Start/stop of a half's stress run: the trace's repeat-until-ack logic
    (a half hears UART0 only while awake), on command 0xBB."""

    def __init__(self, params: dict, **kw) -> None:
        super().__init__(params["duration_s"], **kw)
        self.params = params

    def _start_frame(self) -> bytes:
        return ks_frame(CMD_RF_STRESS, encode_start(self.params))

    def _stop_frame(self) -> bytes:
        return ks_frame(CMD_RF_STRESS, encode_stop())

    def request_stop(self, now: float) -> None:
        if self.state == "idle":           # never started: nothing to stop
            self.state = "off"
            return
        super().request_stop(now)

    def message(self, name: str) -> str:
        if self.state == "refused":
            return (f"{name}: stress start refused (status {self.refused_status}): "
                    f"no radio, half not paired, or parameters out of range")
        return (f"{name}: no answer to {self.attempts} stress start requests in {self.give_up_s:.0f} s")
