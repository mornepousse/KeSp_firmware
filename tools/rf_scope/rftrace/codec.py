"""rf-scope trace frames — the host side of main/comm/rf/rf_trace.{h,c}.

A trace frame is a KR reply of command 0xBA (rationale in rf_trace.h):

    4b 52 ba 00 | len:u16 LE | version kind count | count x record | crc8

CRC-8 = the project's ks_crc8 (poly 0x31, init 0x00, MSB-first, no
reflection) over the payload (version..last record). Records are fixed-size
little-endian; the field names are those of the C structs.

Pinned against the C codec by docs/contracts/rf_trace_vectors.json
(tools/rf_scope/tests/test_codec.py). Stdlib only.
"""

from __future__ import annotations

import struct

MAGIC = b"KR"
CMD = 0xBA
STATUS_OK = 0x00
VERSION = 2          # v2: the periodic record carries `session` and `stress` (26 B)
BATCH_MAX = 16
KR_HDR = 6
PAYLOAD_HDR = 3
HDR = KR_HDR + PAYLOAD_HDR

KIND_TX, KIND_RX, KIND_PERIODIC = 1, 2, 3
KIND_NAMES = {KIND_TX: "tx", KIND_RX: "rx", KIND_PERIODIC: "periodic"}
KIND_IDS = {v: k for k, v in KIND_NAMES.items()}

_RECORDS = {
    KIND_TX: (struct.Struct("<IBBBBBBBBB"),
              ("t_us", "emitter", "seq", "pkt_kind", "payload_len",
               "result", "arc", "plos_delta", "channel", "flags")),
    KIND_RX: (struct.Struct("<IBBBBBBBBB"),
              ("t_us", "emitter", "seq", "pkt_kind", "payload_len",
               "pipe", "rpd", "channel", "fifo_depth", "flags")),
    KIND_PERIODIC: (struct.Struct("<IIBBBBHHHHIH"),
                    ("t_us", "uptime_s", "source", "state", "batt_dV",
                     "channel", "frames", "fails", "dups", "drops", "session", "stress")),
}
REC_SIZE = {k: s.size for k, (s, _) in _RECORDS.items()}
REC_SIZE_MAX = max(REC_SIZE.values())
REC_SIZE_MIN = min(REC_SIZE.values())
FRAME_MAX = HDR + BATCH_MAX * REC_SIZE_MAX + 1

# Identities and flags (rf_trace.h)
EMIT_LEFT, EMIT_RIGHT, EMIT_DONGLE, EMIT_MOUSE = 0, 1, 2, 3
TX_ACK, TX_MAX_RT, TX_TIMEOUT = 0, 1, 2
TXF_EXCURSION, TXF_NOSEQ = 0x01, 0x02
RXF_DELIVERED, RXF_DUP, RXF_NOSEQ = 0x01, 0x02, 0x04
PKT_STRESS = 0xA     # PKT_TYPE_STRESS: synthetic frames of the stress generator
ST_AWAKE, ST_SLEEPING, ST_WOKE = 0, 1, 2


class TraceError(ValueError):
    """A refused frame. `code` matches rf_trace_err_name() in C."""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode_frame(kind: str | int, records: list[dict]) -> bytes:
    k = KIND_IDS[kind] if isinstance(kind, str) else kind
    st, names = _RECORDS[k]
    if not 1 <= len(records) <= BATCH_MAX:
        raise TraceError("count")
    payload = bytes([VERSION, k, len(records)]) + b"".join(
        st.pack(*(r[n] for n in names)) for r in records)
    return (MAGIC + bytes([CMD, STATUS_OK]) + struct.pack("<H", len(payload))
            + payload + bytes([crc8(payload)]))


def _header_check(b: bytes) -> int:
    """Checks the header as far as the bytes go. Returns the total frame
    length once the 9 header bytes are present, 0 before. Raises
    TraceError on the first refusal. Same order as header_check() in C."""
    n = len(b)
    if n > 0 and b[0] != MAGIC[0]:
        raise TraceError("magic")
    if n > 1 and b[1] != MAGIC[1]:
        raise TraceError("magic")
    if n > 2 and b[2] != CMD:
        raise TraceError("cmd")
    if n > 3 and b[3] != STATUS_OK:
        raise TraceError("status")
    if n > 6 and b[6] != VERSION:
        raise TraceError("version")
    if n > 7 and b[7] not in _RECORDS:
        raise TraceError("kind")
    if n > 8 and not 1 <= b[8] <= BATCH_MAX:
        raise TraceError("count")
    if n >= HDR:
        plen = b[4] | (b[5] << 8)
        if plen != PAYLOAD_HDR + b[8] * REC_SIZE[b[7]]:
            raise TraceError("len")
        return KR_HDR + plen + 1
    if n > 5:
        plen = b[4] | (b[5] << 8)
        if not PAYLOAD_HDR + REC_SIZE_MIN <= plen <= PAYLOAD_HDR + BATCH_MAX * REC_SIZE_MAX:
            raise TraceError("len")
    return 0


def _body(b: bytes, total: int) -> dict:
    if crc8(b[KR_HDR:total - 1]) != b[total - 1]:
        raise TraceError("crc")
    kind, count = b[7], b[8]
    st, names = _RECORDS[kind]
    recs = [dict(zip(names, st.unpack_from(b, HDR + i * st.size))) for i in range(count)]
    return {"version": b[6], "kind": KIND_NAMES[kind], "records": recs}


def decode_frame(b: bytes) -> dict:
    """Strict decode of exactly one frame. Raises TraceError(code)."""
    b = bytes(b)
    if len(b) < HDR:
        raise TraceError("short")
    total = _header_check(b)
    if len(b) < total:
        raise TraceError("short")
    if len(b) > total:
        raise TraceError("trailing")
    return _body(b, total)


NONE, FOUND, PARTIAL = "none", "found", "partial"


def scan(buf: bytes, pos: int = 0):
    """Finds the next frame in buf[pos:]. Mirrors rf_trace_scan():
    returns (FOUND, start, length, frame), (PARTIAL, start, 0, None) or
    (NONE, len(buf), 0, None). A refused candidate advances by ONE byte."""
    i = buf.find(MAGIC[0], pos)
    while i != -1:
        try:
            total = _header_check(buf[i:i + HDR])
        except TraceError:
            i = buf.find(MAGIC[0], i + 1)
            continue
        if total == 0 or len(buf) - i < total:
            return PARTIAL, i, 0, None
        try:
            frame = _body(buf[i:i + total], total)
        except TraceError:
            i = buf.find(MAGIC[0], i + 1)
            continue
        return FOUND, i, total, frame
    return NONE, len(buf), 0, None


class StreamScanner:
    """Splits a byte stream (a half's UART0: console text + trace frames, or
    the dongle's CDC) into ("text", bytes) and ("frame", dict) items, in
    order. Feed it chunks of any size."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[str, object]]:
        self._buf += data
        out: list[tuple[str, object]] = []
        buf = bytes(self._buf)
        pos = 0
        while True:
            status, start, length, frame = scan(buf, pos)
            if start > pos:
                out.append(("text", buf[pos:start]))
            if status == FOUND:
                out.append(("frame", frame))
                pos = start + length
                continue
            pos = start
            break
        self._buf = bytearray(buf[pos:])
        return out

    def pending(self) -> bytes:
        """Bytes held back as a possible frame beginning."""
        return bytes(self._buf)
