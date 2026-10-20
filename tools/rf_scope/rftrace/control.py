"""Start/stop of the trace on one source: what to send and when.

The dongle answers a start at once, but a HALF only hears UART0 while awake:
bytes sent while it sleeps (light sleep between keystrokes, the 5 s veille)
are lost. So the start request is REPEATED every `interval_s` until the empty
KR ack (or, if the ack was lost in console text, the first trace frame) comes
back; a start that lands while the trace is on merely restarts it. If nothing
answers within `give_up_s` the half is asleep for good: say so.

Pure: no I/O, time is passed in (seconds, any monotonic origin).
"""

from __future__ import annotations

from .codec import crc8

CMD_RF_TRACE = 0xBA
DEFAULT_GIVE_UP_S = 20.0


def ks_frame(cmd: int, payload: bytes = b"") -> bytes:
    return b"KS" + bytes([cmd]) + len(payload).to_bytes(2, "little") + payload + bytes([crc8(payload)])


class StartController:
    def __init__(self, duration_s: int, interval_s: float = 0.25, give_up_s: float = DEFAULT_GIVE_UP_S,
                 stop_budget_s: float = 3.0) -> None:
        self.duration_s = max(1, min(int(duration_s), 65535))
        self.interval_s = interval_s
        self.give_up_s = give_up_s
        self.stop_budget_s = stop_budget_s
        self.state = "idle"       # idle | starting | on | gave_up | stopping | off
        self.attempts = 0
        self._t0 = 0.0
        self._next = 0.0
        self._stop_t0 = 0.0

    def poll(self, now: float) -> bytes | None:
        """The bytes to write now, or None."""
        if self.state == "idle":
            self.state, self._t0, self._next = "starting", now, now
        if self.state == "starting":
            if now - self._t0 > self.give_up_s:
                self.state = "gave_up"
                return None
            if now >= self._next:
                self.attempts += 1
                self._next = now + self.interval_s
                return ks_frame(CMD_RF_TRACE, b"\x01" + self.duration_s.to_bytes(2, "little"))
        elif self.state == "stopping":
            if now - self._stop_t0 > self.stop_budget_s:
                self.state = "off"
                return None
            if now >= self._next:
                self._next = now + self.interval_s
                return ks_frame(CMD_RF_TRACE, b"\x00")
        return None

    def on_ack(self, now: float) -> None:
        if self.state == "starting":
            self.state = "on"
        elif self.state == "stopping":
            self.state = "off"

    def on_data(self, now: float) -> None:
        if self.state == "starting":
            self.state = "on"

    def request_stop(self, now: float) -> None:
        if self.state in ("starting", "on", "idle"):
            self.state = "stopping"
            self._stop_t0 = now
            self._next = now

    def message(self, name: str) -> str:
        return (f"{name}: no answer to {self.attempts} start requests in {self.give_up_s:.0f} s — "
                f"the half is probably asleep (it hears UART0 only while awake): "
                f"press a key on the half to wake it, then run the capture again")
