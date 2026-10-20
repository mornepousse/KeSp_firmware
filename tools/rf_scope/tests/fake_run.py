#!/usr/bin/env python3
"""Synthetic run.jsonl: two halves typing and sleeping, a dongle receiving.

Realistic enough to demo `replay` / `report` without hardware, and a fixture
for the tests. Deterministic for a given seed. What it simulates:

* left and right halves: typing bursts (presses, releases, held keys
  re-asserted every 100 ms), 5 s awake-idle, then light sleep 6-20 s with the
  SLEEPING / WOKE periodic records; one STATUS frame per burst; the left also
  emits NOSEQ HIDREPORT frames;
* an 8-bit seq per emitter that wraps, a 32-bit t_us that wraps on the left at
  `wrap_at_s`, boot times far apart (the three clocks are unrelated), a drift
  of +25 ppm / -18 ppm of the halves against the dongle;
* radio: retries (arc), MAX_RT where a quarter of the frames arrive anyway
  (lost ACK), silent losses, duplicates, a burst of 8 consecutive frames lost on the right (mid-run), delay =
  0.8 ms + exp(1.5 ms) with 5 % of 10-40 ms stragglers, RPD bit;
* host arrival: halves go through a 16 ms-quantised UART/USB latency timer, the
  dongle through ~1 ms; one trace-ring drop on the left;
* ctl exchange (start repeated twice on the left, which was asleep) and a few
  console lines.
"""

import argparse
import json
import math
import random
import sys

W = 1 << 32
NS = 1_000_000_000
HOST0 = 5_000_000_000_000
DONGLE_BOOT_US = 216_000_000_000          # 2.5 days
CHANNEL = 82


def generate(path, seed=1, seconds=120, wrap_at_s=50):
    rnd = random.Random(seed)
    items = []                                         # (host_ns, order, item)
    n = [0]

    def put(host_ns, item):
        n[0] += 1
        items.append((int(host_ns), n[0], item))

    boots = {0: W - wrap_at_s * 1_000_000, 1: 5 * 86_400 * 1_000_000 + 123_456}
    drift = {0: 25e-6, 1: -18e-6}
    ports = {"dongle": "/dev/ttyACM0", "left": "/dev/ttyUSB0", "right": "/dev/ttyUSB1"}
    srcs = {0: "left", 1: "right"}

    def half_t(e, tau):                                # true time (s) -> half clock (us, full)
        return boots[e] + tau * 1e6 * (1 + drift[e])

    def dongle_t(tau):
        return DONGLE_BOOT_US + tau * 1e6

    def quant(x, q):
        return math.ceil(x / q) * q

    def half_host(tau):
        return HOST0 + (quant(tau + rnd.uniform(0.002, 0.018), 0.016)) * NS

    def dongle_host(tau):
        return HOST0 + quant(tau + rnd.uniform(0.0005, 0.003), 0.001) * NS

    put(HOST0 - 1, {"type": "meta", "version": 1, "tool": "fake_run", "seed": seed,
                    "sources": ports, "note": "synthetic run (tests/fake_run.py)"})

    # ---- ctl exchange ---------------------------------------------------------
    put(HOST0, {"type": "ctl", "src": "dongle", "port": ports["dongle"], "host_ns": HOST0,
                "event": "start_sent", "duration_s": seconds + 30})
    put(HOST0 + 3_000_000, {"type": "ctl", "src": "dongle", "port": ports["dongle"],
                            "host_ns": HOST0 + 3_000_000, "event": "ack", "attempts": 1})
    for e, delay in ((0, 0.5), (1, 0.0)):
        s = srcs[e]
        put(HOST0, {"type": "ctl", "src": s, "port": ports[s], "host_ns": HOST0,
                    "event": "start_sent", "duration_s": seconds + 30})
        h = HOST0 + int((delay + 0.012) * NS)
        put(h, {"type": "ctl", "src": s, "port": ports[s], "host_ns": h, "event": "ack",
                "attempts": 3 if delay else 1})
        put(h + 2_000_000, {"type": "text", "src": s, "host_ns": h + 2_000_000,
                            "text": f"W ({int(boots[e] % 10**7)}) rftrace: ON for {seconds + 30} s (session 1)"})

    # ---- the halves -------------------------------------------------------------
    tx_total = {0: 0, 1: 0}
    dongle_rx = []                                     # (tau_rx, record fields, flags)
    for e in (0, 1):
        s = srcs[e]
        seq = rnd.randrange(256)
        ring_frames = 0
        ring_fails = 0
        frames = []                                    # (tau, pkt_kind, noseq)
        sleeps = []                                    # (tau_sleep, tau_wake)
        t = 0.6 + 2.2 * e
        while t < seconds:
            end = t + rnd.uniform(4, 10)
            frames.append((t, 6, False))               # STATUS on the first key
            while t < end:
                hold = 0.08 + rnd.expovariate(1 / 0.05)
                if rnd.random() < 0.08:
                    hold = rnd.uniform(0.4, 1.2)
                frames.append((t, 4, False))
                k = 1
                while hold > 0.25 and k * 0.1 < hold:
                    frames.append((t + k * 0.1, 4, False))
                    k += 1
                frames.append((t + hold, 4, False))
                if e == 0 and rnd.random() < 0.5:
                    frames.append((t + hold + 0.004, 5, True))
                t += hold + rnd.uniform(0.05, 0.25)
            t += 5.0
            dur = rnd.uniform(6, 20)
            if t < seconds:
                sleeps.append((t, t + dur))
            t += dur + 0.02
        frames.sort()

        def asleep_at(tau):
            return any(a <= tau < b for a, b in sleeps)

        retry_w = [0.85, 0.08, 0.04, 0.02, 0.01]
        mid_i = len(frames) // 2
        out_idx = range(mid_i, mid_i + 8) if e == 1 else range(0)        # 8 frames in a row lost on the right
        for fi, (tau, kind, noseq) in enumerate(frames):
            if tau >= seconds:
                continue
            maxrt = rnd.random() < 0.012
            arc = 15 if maxrt else rnd.choices(range(5), retry_w)[0]
            outage = fi in out_idx
            delivered = (not outage) and (rnd.random() > 0.004) and ((not maxrt) or rnd.random() < 0.25)
            ring_frames += 1
            ring_fails += 1 if maxrt else 0
            tx_total[e] += 1
            rec_seq = 0 if noseq else seq
            if not noseq:
                seq = (seq + 1) % 256
            tau_done = tau + 0.0008 * (1 + arc)
            tt = half_t(e, tau_done)
            flags = 0x02 if noseq else 0
            if e == 0 and kind == 4 and rnd.random() < 0.1:
                flags |= 0x01
            rec = {"t_us": int(tt) % W, "emitter": e, "seq": rec_seq, "pkt_kind": kind,
                   "payload_len": 6 if kind == 4 else 8, "result": 1 if maxrt else 0, "arc": arc,
                   "plos_delta": 2 if maxrt else 0, "channel": CHANNEL, "flags": flags}
            put(half_host(tau_done), {"type": "rec", "src": s, "port": ports[s], "kind": "tx", **rec,
                                      "host_ns": 0})
            items[-1][2]["host_ns"] = items[-1][0]
            if delivered:
                lat = 0.0008 + rnd.expovariate(1 / 0.0015)
                if rnd.random() < 0.05:
                    lat += rnd.uniform(0.010, 0.040)
                tau_rx = tau_done + lat
                strong = rnd.random() < (0.92 if e == 0 else 0.8)
                dongle_rx.append((tau_rx, {"emitter": e, "seq": rec_seq, "pkt_kind": kind,
                                           "payload_len": rec["payload_len"], "pipe": 0, "rpd": int(strong),
                                           "channel": CHANNEL, "fifo_depth": 1 if rnd.random() < 0.97 else 2,
                                           "flags": 0 if noseq else 0x01}))
                if rnd.random() < 0.01:
                    dongle_rx.append((tau_rx + rnd.uniform(0.004, 0.010), {
                        "emitter": e, "seq": rec_seq, "pkt_kind": kind, "payload_len": rec["payload_len"],
                        "pipe": 0, "rpd": int(strong), "channel": CHANNEL, "fifo_depth": 1, "flags": 0x02}))

        # periodic records (1 Hz while awake; SLEEPING on the way in, WOKE on the way out)
        counts = {}
        for tau, _k, _n in frames:
            counts.setdefault(int(tau), [0, 0])[0] += 1

        def periodic(tau, state, drops=0):
            tt = half_t(e, tau)
            sec = int(tau)
            fr = counts.get(sec, [0, 0])[0]
            rec = {"t_us": int(tt) % W, "uptime_s": int(tt // 1_000_000), "source": e, "state": state,
                   "batt_dV": 41 - int(tau // 90), "channel": CHANNEL, "frames": fr, "fails": 0,
                   "dups": 0, "drops": drops}
            put(half_host(tau), {"type": "rec", "src": s, "port": ports[s], "kind": "periodic", **rec,
                                 "host_ns": 0})
            items[-1][2]["host_ns"] = items[-1][0]

        for sec in range(0, seconds):
            if not asleep_at(sec + 0.0):
                periodic(float(sec), 0, drops=3 if (e == 0 and sec == 45) else 0)
        for a, b in sleeps:
            periodic(a, 1)
            periodic(min(b + 0.03, seconds - 0.01), 2)
        put(HOST0 + (5 + e) * NS, {"type": "text", "src": s, "port": ports[s], "host_ns": HOST0 + (5 + e) * NS,
                                   "text": f"HB up={int(half_t(e, 5) // 1e6)} idle=0 slept={len(sleeps)}"})

    # ---- the dongle ---------------------------------------------------------------
    dongle_rx.sort(key=lambda x: x[0])
    per_sec = {}
    for tau, rec in dongle_rx:
        if tau >= seconds:
            continue
        r = dict(rec, t_us=int(dongle_t(tau)) % W)
        put(dongle_host(tau), {"type": "rec", "src": "dongle", "port": ports["dongle"], "kind": "rx", **r,
                               "host_ns": 0})
        items[-1][2]["host_ns"] = items[-1][0]
        c = per_sec.setdefault(int(tau), [0, 0])
        c[0] += 1
        c[1] += 1 if rec["flags"] & 2 else 0
    for sec in range(0, seconds + 1):
        tt = dongle_t(sec + 0.5)
        f, d = per_sec.get(sec, [0, 0])
        put(dongle_host(sec + 0.5), {"type": "rec", "src": "dongle", "port": ports["dongle"], "kind": "periodic",
                                     "t_us": int(tt) % W, "uptime_s": int(tt // 1_000_000), "source": 2,
                                     "state": 0, "batt_dV": 0, "channel": CHANNEL, "frames": f, "fails": 0,
                                     "dups": d, "drops": 0, "host_ns": 0})
        items[-1][2]["host_ns"] = items[-1][0]
    end = HOST0 + int((seconds + 1) * NS)
    for s in ("dongle", "left", "right"):
        put(end, {"type": "ctl", "src": s, "port": ports[s], "host_ns": end, "event": "stop_sent"})
        put(end + 4_000_000, {"type": "ctl", "src": s, "port": ports[s], "host_ns": end + 4_000_000,
                              "event": "stop_ack"})

    items.sort(key=lambda x: (x[0], x[1]))
    with open(path, "w", encoding="utf-8") as f:
        for _h, _o, it in items:
            f.write(json.dumps(it, ensure_ascii=False, separators=(",", ":")) + "\n")
    return path


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--wrap-at", type=int, default=50, help="second at which the left's t_us wraps")
    a = ap.parse_args()
    generate(a.output, a.seed, a.seconds, a.wrap_at)
    print(f"wrote {a.output}", file=sys.stderr)
