"""Run files: one JSON object per line.

    {"type": "meta", "version": 1, "sources": {...}, ...}          first line
    {"type": "rec",  "src": "left", "port": "/dev/ttyUSB0", "host_ns": N,
     "kind": "tx|rx|periodic", <the record's fields as in rf_trace.h>}
    {"type": "text", "src": ..., "host_ns": N, "text": "console line"}
    {"type": "ctl",  "src": ..., "host_ns": N, "event": "start_sent|ack|giveup|
                     stop_sent|stop_ack|closed", ...}

`host_ns` is time.monotonic_ns() when the bytes carrying the item were read
(all records of one chunk share it). A run is self-contained: console text and
the control exchange are in the file.
"""

from __future__ import annotations

import json
import os
from typing import Iterator

VERSION = 1


class Writer:
    def __init__(self, path, flush_every: int = 200) -> None:
        self._f = open(path, "w", encoding="utf-8")
        self._n = 0
        self._flush_every = flush_every

    def write(self, item: dict) -> None:
        self._f.write(json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n")
        self._n += 1
        if self._n % self._flush_every == 0:
            self._f.flush()

    def close(self) -> None:
        if not self._f.closed:
            self._f.flush()
            self._f.close()

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def read(path) -> Iterator[dict]:
    """Yields the items; a truncated last line (killed capture) is ignored."""
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                return


def frame_items(frame: dict, src: str, port: str, host_ns: int) -> list[dict]:
    return [{"type": "rec", "src": src, "port": port, "host_ns": host_ns,
             "kind": frame["kind"], **r} for r in frame["records"]]
