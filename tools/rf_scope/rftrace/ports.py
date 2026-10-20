"""Port safety: never open a busy port, never touch DTR/RTS of a half's.

* `check_port_free`: refuses a port another process holds (fuser if present,
  else a /proc/*/fd scan) — two readers of one tty split its bytes between
  them and corrupt both captures.
* `open_serial(half=True)`: an ESP-Prog wires DTR/RTS to EN/IO0; leaving them
  asserted resets or holds the board. The Serial object is built UNOPENED,
  dtr/rts are set low, then it opens; they are never written again. (The
  kernel may still raise both for an instant inside open(2), before pyserial
  can lower them: unavoidable from userland, and what the proven
  dtr=rts=False recipe already lives with.) The dongle's CDC port keeps
  pyserial's defaults, like scripts/kesp_cdc.py.
"""

from __future__ import annotations

import os
import shutil
import subprocess


class PortBusy(RuntimeError):
    pass


def _comm_of(pid: int, proc_root: str = "/proc") -> str:
    try:
        with open(f"{proc_root}/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return "?"


def holders_from_fuser(path: str) -> list[tuple[int, str]]:
    r = subprocess.run(["fuser", path], capture_output=True, text=True, timeout=5)
    pids = [int(t) for t in r.stdout.split() if t.isdigit()]
    return [(p, _comm_of(p)) for p in pids if p != os.getpid()]


def holders_from_proc(path: str, proc_root: str = "/proc") -> list[tuple[int, str]]:
    target = os.path.realpath(path)
    found: list[tuple[int, str]] = []
    try:
        pids = [d for d in os.listdir(proc_root) if d.isdigit()]
    except OSError:
        return found
    for d in pids:
        pid = int(d)
        if pid == os.getpid():
            continue
        fd_dir = f"{proc_root}/{d}/fd"
        try:
            for fd in os.listdir(fd_dir):
                try:
                    if os.path.realpath(os.readlink(f"{fd_dir}/{fd}")) == target:
                        found.append((pid, _comm_of(pid, proc_root)))
                        break
                except OSError:
                    continue
        except OSError:
            continue
    return sorted(found)


def port_holders(path: str) -> list[tuple[int, str]]:
    if shutil.which("fuser"):
        try:
            return holders_from_fuser(path)
        except (OSError, subprocess.SubprocessError):
            pass
    return holders_from_proc(path)


def check_port_free(path: str, holders_fn=None) -> None:
    holders = (holders_fn or port_holders)(path)
    if holders:
        who = ", ".join(f"{name} (pid {pid})" for pid, name in holders)
        raise PortBusy(f"{path} is in use by {who} — refusing to open it; close that program first")


def open_serial(path: str, baud: int, half: bool, serial_factory=None, timeout: float = 0.05):
    if serial_factory is None:
        import serial
        serial_factory = serial.Serial
    s = serial_factory()
    s.port = path
    s.baudrate = baud
    s.timeout = timeout
    if half:
        s.dtr = False
        s.rts = False
    s.open()
    return s
