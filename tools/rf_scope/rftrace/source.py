"""One capture source: a serial port, its start controller and its stream.

`pump()` is one iteration of the reader thread, kept free of threads and of
pyserial so tests drive it with a fake port and a fake clock:
read what is there -> decode -> items to the sink -> maybe write a request.
Reads are handled BEFORE the controller is polled, so an ack that is already
in the buffer cancels the next repeat.
"""

from __future__ import annotations

import time
from typing import Callable

from . import jsonl
from .control import StartController
from .stream import SourceStream


class SourceRunner:
    def __init__(self, src: str, port_name: str, port, ctl: StartController, sink: Callable[[dict], None],
                 clock_mono: Callable[[], float] = time.monotonic,
                 clock_ns: Callable[[], int] = time.monotonic_ns) -> None:
        self.src, self.port_name, self.port, self.ctl, self.sink = src, port_name, port, ctl, sink
        self.mono, self.ns = clock_mono, clock_ns
        self.stream = SourceStream()
        self.done = False
        self._gave_up_sent = False
        self._start_logged = False
        self._stop_logged = False

    def _emit(self, **kw) -> None:
        self.sink({"type": "ctl", "src": self.src, "port": self.port_name, "host_ns": self.ns(), **kw})

    def request_stop(self) -> None:
        self.ctl.request_stop(self.mono())

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
        if self.ctl.state == "gave_up" and not self._gave_up_sent:
            self._gave_up_sent = True
            self._emit(event="giveup", attempts=self.ctl.attempts, message=self.ctl.message(self.src))
        if self.ctl.state == "off":
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
        elif kind == "ack":
            was = self.ctl.state
            self.ctl.on_ack(self.mono())
            if was == "starting":
                self._emit(event="ack", attempts=self.ctl.attempts)
            elif was == "stopping":
                self._emit(event="stop_ack")
        elif kind == "line":
            self.sink({"type": "text", "src": self.src, "host_ns": host_ns, "text": item[1]})

    def flush(self) -> None:
        for kind, val in self.stream.flush():
            self.sink({"type": "text", "src": self.src, "host_ns": self.ns(), "text": val})
