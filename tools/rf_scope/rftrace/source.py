"""One capture source: a serial port, its start controller and its stream.

`pump()` is one iteration of the reader thread, kept free of threads and of
pyserial so tests drive it with a fake port and a fake clock:
read what is there -> decode -> items to the sink -> maybe write a request.
Reads are handled BEFORE the controller is polled, so an ack that is already
in the buffer cancels the next repeat.

A half's runner may also carry a stress controller (`rf_scope stress`): its
start is sent only once the trace is on (every stress frame then has its TX
record), and a stop request stops the STRESS first and the trace only once
the stress is off (its last frames and its final periodic record are traced).
"""

from __future__ import annotations

import time
from typing import Callable

from . import jsonl
from .control import StartController
from .stream import CMD_RF_STRESS, CMD_RF_TRACE, SourceStream


class SourceRunner:
    def __init__(self, src: str, port_name: str, port, ctl: StartController, sink: Callable[[dict], None],
                 clock_mono: Callable[[], float] = time.monotonic,
                 clock_ns: Callable[[], int] = time.monotonic_ns, stress=None) -> None:
        self.src, self.port_name, self.port, self.ctl, self.sink = src, port_name, port, ctl, sink
        self.stress = stress                       # StressController or None
        self.mono, self.ns = clock_mono, clock_ns
        self.stream = SourceStream()
        self.done = False
        self._gave_up_sent = False
        self._start_logged = False
        self._stop_logged = False
        self._s_logged: set[str] = set()

    def _emit(self, **kw) -> None:
        self.sink({"type": "ctl", "src": self.src, "port": self.port_name, "host_ns": self.ns(), **kw})

    def request_stop(self) -> None:
        now = self.mono()
        if self.stress is not None:
            if self.stress.state in ("idle", "starting", "on"):
                self.stress.request_stop(now)
                if self.stress.state != "off":
                    return                         # the trace waits for the stress to be off
            if self.stress.state == "stopping":
                return
        self.ctl.request_stop(now)

    def pump(self) -> None:
        n = self.port.in_waiting
        data = self.port.read(n) if n else b""
        host_ns = self.ns()
        for item in self.stream.feed(data) if data else []:
            self._handle(item, host_ns)
        now = self.mono()
        before = self.ctl.state
        out = self.ctl.poll(now)
        if out is not None:
            self.port.write(out)
            if before in ("idle", "starting") and not self._start_logged:
                self._start_logged = True
                self._emit(event="start_sent", duration_s=self.ctl.duration_s)
            elif self.ctl.state == "stopping" and not self._stop_logged:
                self._stop_logged = True
                self._emit(event="stop_sent")
        if self.stress is not None and self.ctl.state == "on":
            self._pump_stress(now)
        if self.ctl.state == "gave_up" and not self._gave_up_sent:
            self._gave_up_sent = True
            self._emit(event="giveup", attempts=self.ctl.attempts, message=self.ctl.message(self.src))
        if self.ctl.state in ("off", "refused"):
            self.done = True

    def _handle(self, item: tuple, host_ns: int) -> None:
        kind = item[0]
        if kind == "frame":
            was = self.ctl.state
            self.ctl.on_data(self.mono())
            if was == "starting":                      # the ack was lost in console text
                self._emit(event="ack", attempts=self.ctl.attempts, via="data")
            for it in jsonl.frame_items(item[1], self.src, self.port_name, host_ns):
                self.sink(it)
        elif kind == "reply":
            self._reply(item[1], item[2], item[3])
        elif kind == "line":
            self.sink({"type": "text", "src": self.src, "host_ns": host_ns, "text": item[1]})

    def _pump_stress(self, now: float) -> None:
        st = self.stress
        before = st.state
        out = st.poll(now)
        if out is not None:
            self.port.write(out)
            if before in ("idle", "starting") and "start" not in self._s_logged:
                self._s_logged.add("start")
                self._emit(event="stress_start_sent", scenario=st.params["scenario"], params=dict(st.params))
            elif st.state == "stopping" and "stop" not in self._s_logged:
                self._s_logged.add("stop")
                self._emit(event="stress_stop_sent")
        if st.state == "gave_up" and "giveup" not in self._s_logged:
            self._s_logged.add("giveup")
            self._emit(event="stress_giveup", attempts=st.attempts, message=st.message(self.src))

    def _stress_reply(self, status: int, info: dict | None) -> None:
        st = self.stress
        was = st.state
        if status != 0:
            st.on_refused(self.mono(), status)
            if was == "starting":
                self._emit(event="stress_refused", status=status, message=st.message(self.src))
            return
        st.on_ack(self.mono(), info)
        extra = {"session": info["session"], "remaining_s": info["remaining_s"]} if info else {}
        if was == "starting":
            self._emit(event="stress_ack", attempts=st.attempts, **extra)
        elif was == "stopping":
            self._emit(event="stress_stop_ack", **extra)

    def _reply(self, cmd: int, status: int, info: dict | None) -> None:
        if cmd == CMD_RF_STRESS and self.stress is not None:
            self._stress_reply(status, info)
            return
        if cmd != CMD_RF_TRACE:
            return
        was = self.ctl.state
        if status != 0:
            self.ctl.on_refused(self.mono(), status)
            if was == "starting":
                self._emit(event="refused", status=status)
            return
        self.ctl.on_ack(self.mono(), info)
        extra = {"session": info["session"], "remaining_s": info["remaining_s"]} if info else {}
        if was == "starting":
            self._emit(event="ack", attempts=self.ctl.attempts, **extra)
        elif was == "stopping":
            self._emit(event="stop_ack", **extra)

    def flush(self) -> None:
        for kind, val in self.stream.flush():
            self.sink({"type": "text", "src": self.src, "host_ns": self.ns(), "text": val})
