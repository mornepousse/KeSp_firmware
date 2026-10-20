"""One source's byte stream -> ("frame", dict) | ("reply", cmd, status, ack) | ("line", str).

The trace frames are found by codec.StreamScanner; everything else is text.
Command replies are KR frames of RF_TRACE (0xBA) or RF_STRESS (0xBB) with a
payload SHORTER than any trace frame's (< 16 bytes): the scanner (rightly)
does not take them for trace frames, so they surface as text bytes and are
cut out here, wherever chunking splits them. A start/stop ack carries
[sub][session u32 LE][remaining_s u16 LE] (rf_trace_ctl.h: RF_TRACE_ACK_LEN);
an error reply carries nothing. The ack's bytes may include 0x0A: a
plausible reply that is not complete yet is HELD BACK, and lines are split
only before it — a newline inside a reply never cuts the console text.
Console lines are split on \\n (\\r dropped); a long unterminated run is
flushed as a line so a silent half cannot grow the buffer.
"""

from __future__ import annotations

from .codec import StreamScanner, crc8

CMD_RF_TRACE, CMD_RF_STRESS = 0xBA, 0xBB
REPLY_CMDS = (CMD_RF_TRACE, CMD_RF_STRESS)
REPLY_PAYLOAD_MAX = 15        # a trace frame's payload is >= 16 bytes
ACK_LEN = 7
LINE_MAX = 2048


def kr_reply(cmd: int, status: int, payload: bytes = b"") -> bytes:
    """A KR reply, byte for byte what the firmware writes (ks_respond)."""
    return b"KR" + bytes([cmd, status]) + len(payload).to_bytes(2, "little") + payload + bytes([crc8(payload)])


ACK = kr_reply(CMD_RF_TRACE, 0)   # the pre-v2 firmware's empty ack: still an ack


def parse_ack(payload: bytes) -> dict | None:
    if len(payload) != ACK_LEN:
        return None
    return {"sub": payload[0], "session": int.from_bytes(payload[1:5], "little"),
            "remaining_s": int.from_bytes(payload[5:7], "little")}


def _reply_check(b: bytes):
    """("bad", 0) | ("partial", 0) | ("ok", total) for a candidate at b[0]."""
    n = len(b)
    if n > 0 and b[0] != 0x4B:
        return "bad", 0
    if n > 1 and b[1] != 0x52:
        return "bad", 0
    if n > 2 and b[2] not in REPLY_CMDS:
        return "bad", 0
    if n < 6:
        return "partial", 0
    plen = b[4] | (b[5] << 8)
    if plen > REPLY_PAYLOAD_MAX:
        return "bad", 0
    total = 6 + plen + 1
    if n < total:
        return "partial", 0
    if crc8(b[6:6 + plen]) != b[6 + plen]:
        return "bad", 0
    return "ok", total


class SourceStream:
    def __init__(self) -> None:
        self._sc = StreamScanner()
        self._text = bytearray()

    def _replies(self, out: list) -> int:
        """Cuts every complete reply out of the text; returns where a still
        incomplete one begins (len(text) when none)."""
        t = self._text
        i = t.find(b"K")
        hold = len(t)
        while i != -1:
            st, total = _reply_check(bytes(t[i:i + 6 + REPLY_PAYLOAD_MAX + 1]))
            if st == "ok":
                cmd, status = t[i + 2], t[i + 3]
                payload = bytes(t[i + 6:i + total - 1])
                out.append(("reply", cmd, status, parse_ack(payload) if status == 0 else None))
                del t[i:i + total]
                i = t.find(b"K", i)
                continue
            if st == "partial":
                hold = i
                break
            i = t.find(b"K", i + 1)
        return hold

    def feed(self, data: bytes) -> list[tuple]:
        out: list[tuple] = []
        for kind, val in self._sc.feed(data):
            if kind == "frame":
                out.append(("frame", val))
            else:
                self._text += val
        hold = self._replies(out)
        while True:
            i = self._text.find(b"\n", 0, hold)
            if i < 0:
                break
            out.append(("line", self._decode(self._text[:i])))
            del self._text[:i + 1]
            hold -= i + 1
        if hold > LINE_MAX:
            out.append(("line", self._decode(self._text[:hold])))
            del self._text[:hold]
        return [o for o in out if o != ("line", "")]

    def flush(self) -> list[tuple]:
        out = [("line", self._decode(self._text))] if self._text.strip() else []
        self._text.clear()
        return out

    @staticmethod
    def _decode(b: bytes) -> str:
        return bytes(b).decode("utf-8", errors="replace").rstrip("\r")
