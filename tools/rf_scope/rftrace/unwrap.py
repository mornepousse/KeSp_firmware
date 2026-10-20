"""Unwrapping of the 32-bit `t_us` of one device into a monotonic timeline.

t_us is the emitter's esp_timer truncated to 32 bits: it wraps every 71.6 min.
Two tools:

* a *signed-delta* walk: each record is placed within +-35 min of the previous
  one (a small backward step is a reordered record, not a wrap);
* an *anchor* from a periodic record, which carries `uptime_s` taken at the
  same instant: it fixes the epoch outright, whatever happened in between
  (a sleep of hours included).

After a SLEEPING record the next plain record is placed FORWARD only: a sleep
longer than 35 min would otherwise be mistaken for a step back.
"""

from __future__ import annotations

W = 1 << 32
HALF = 1 << 31


class Unwrapper:
    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.head: int | None = None      # largest unwrapped value so far
        self.forward_only = False

    def mark_sleep(self) -> None:
        self.forward_only = True

    def anchor(self, t32: int, uptime_s: int) -> int:
        """Periodic record: t32 and uptime_s were read at the same instant, so
        t = uptime_s * 1e6 + [0, 1e6). Picks the epoch k that fits."""
        k = round((uptime_s * 1_000_000 + 500_000 - t32) / W)
        full = t32 + max(k, 0) * W
        self.head = full
        self.forward_only = False
        return full

    def unwrap(self, t32: int) -> int:
        if self.head is None:
            self.head = t32
            return t32
        d = (t32 - self.head) % W
        step = d if (self.forward_only or d < HALF) else d - W
        self.forward_only = False
        full = self.head + step
        if full > self.head:
            self.head = full
        return full
