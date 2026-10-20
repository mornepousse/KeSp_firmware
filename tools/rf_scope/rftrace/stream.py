"""One source's byte stream -> ("frame", dict) | ("ack",) | ("line", str).

The trace frames are found by codec.StreamScanner; everything else is text.
The start/stop acknowledgement is an EMPTY KR 0xBA (`4b 52 ba 00 00 00 00`),
which the scanner (rightly) does not take for a trace frame, so it surfaces as
text bytes: it is cut out of the text here, wherever chunking splits it.
Console lines are split on \\n (\\r dropped); a long unterminated run is
flushed as a line so a silent half cannot grow the buffer.
"""

from __future__ import annotations

from .codec import StreamScanner

ACK = bytes([0x4B, 0x52, 0xBA, 0x00, 0x00, 0x00, 0x00])
LINE_MAX = 2048


class SourceStream:
    def __init__(self) -> None:
        self._sc = StreamScanner()
        self._text = bytearray()

    def feed(self, data: bytes) -> list[tuple]:
        out: list[tuple] = []
        for kind, val in self._sc.feed(data):
            if kind == "frame":
                out.append(("frame", val))
            else:
                self._text += val
        while True:
            i = self._text.find(ACK)
            if i < 0:
                break
            del self._text[i:i + len(ACK)]
            out.append(("ack",))
        while True:
            i = self._text.find(b"\n")
            if i < 0:
                break
            out.append(("line", self._decode(self._text[:i])))
            del self._text[:i + 1]
        if len(self._text) > LINE_MAX:
            out.append(("line", self._decode(self._text)))
            self._text.clear()
        return [o for o in out if o != ("line", "")]

    def flush(self) -> list[tuple]:
        out = [("line", self._decode(self._text))] if self._text.strip() else []
        self._text.clear()
        return out

    @staticmethod
    def _decode(b: bytes) -> str:
        return bytes(b).decode("utf-8", errors="replace").rstrip("\r")
