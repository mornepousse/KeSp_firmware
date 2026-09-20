#!/usr/bin/env python3
"""Capture a board's UART console (FTDI / ESP-Prog) WITHOUT holding it in reset:
DTR and RTS are released before the port opens — `cat /dev/ttyUSBx` or a
default serial terminal asserts them and keeps the ESP32-S3 in reset.
Each line is prefixed with the wall-clock time (the ESP log timestamp follows
the FreeRTOS tick, which stops during light sleep).

    scripts/console-capture.py /dev/ttyUSB2 left.log [duration_s]

Run it detached for long sessions (a night on battery):
    setsid nohup scripts/console-capture.py /dev/ttyUSB2 night.log 43200 &
"""
import sys, time
import serial

port, out = sys.argv[1], sys.argv[2]
duration = float(sys.argv[3]) if len(sys.argv) > 3 else 12 * 3600
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.5
s.dtr = False
s.rts = False
s.open()
t0, buf = time.time(), b""
with open(out, "ab", buffering=0) as f:
    f.write(f"--- capture {time.strftime('%Y-%m-%d %H:%M:%S')} {port}\n".encode())
    while time.time() - t0 < duration:
        try:
            chunk = s.read(4096)
        except serial.SerialException as e:
            f.write(f"--- port lost {time.strftime('%H:%M:%S')}: {e}\n".encode())
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            f.write(time.strftime("%H:%M:%S").encode() + b"." + f"{int((time.time() % 1) * 1000):03d} ".encode() + line + b"\n")
