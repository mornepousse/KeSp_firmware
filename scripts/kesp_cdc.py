#!/usr/bin/env python3
"""Small bench client for the binary CDC protocol (docs/CDC_BINARY_PROTOCOL.md).

    scripts/kesp_cdc.py PORT version            firmware version (any board)
    scripts/kesp_cdc.py PORT dfu                reboot into the ROM download mode (then esptool on the same port)
    scripts/kesp_cdc.py PORT rfstat             dongle: link, counters, engine and USB diagnostics (RF_STATUS, 51 bytes)
    scripts/kesp_cdc.py PORT pair [reset]       dongle: open the 30 s pairing window (reset=1 forgets the pairs first)
    scripts/kesp_cdc.py PORT pairs              dongle: list the paired halves

PORT is the board's CDC port (/dev/ttyACM* on Linux; on a Niphargus half the
CDC is on its USB-C, the FTDI header is the console). Needs pyserial.
"""
import struct, sys, time
import serial

KS_CMD = {"version": 0x01, "dfu": 0x03, "pair": 0xB2, "rfstat": 0xB3, "pairs": 0xB4}


def crc8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xFF if (c & 0x80) else (c << 1) & 0xFF
    return c


def request(port: str, cmd: int, payload: bytes = b"", wait: float = 0.4) -> bytes:
    """One KS frame, returns the KR payload (raises on a non-OK status)."""
    s = serial.Serial(port, 115200, timeout=2)
    time.sleep(0.3)
    s.reset_input_buffer()
    hdr = bytes([0x4B, 0x53, cmd]) + struct.pack("<H", len(payload))
    s.write(hdr + payload + bytes([crc8(payload)]))   # CRC-8 over the payload only
    s.flush()
    time.sleep(wait)
    r = s.read(512)
    s.close()
    i = r.find(bytes([0x4B, 0x52, cmd]))
    if i < 0:
        raise SystemExit(f"no KR answer for command 0x{cmd:02X} ({len(r)} bytes read)")
    status = r[i + 3]
    n = struct.unpack("<H", r[i + 4:i + 6])[0]
    if status != 0:
        raise SystemExit(f"command 0x{cmd:02X}: status 0x{status:02X}")
    return r[i + 6:i + 6 + n]


def rfstat(p: bytes) -> str:
    flags, sig_kbd = p[0], p[1]
    age_k, age_m, rx_k, rx_m, dup_k, dup_m = struct.unpack("<6I", p[3:27])
    overwritten = struct.unpack("<I", p[27:31])[0] if len(p) >= 31 else -1
    gap = struct.unpack("<I", p[31:35])[0] if len(p) >= 35 else -1
    usb = struct.unpack("<4H", p[35:43]) if len(p) >= 43 else (-1, -1, -1, -1)
    repress = struct.unpack("<I", p[43:47])[0] if len(p) >= 47 else -1
    rh, rk, rms = (p[47], p[48], struct.unpack("<H", p[49:51])[0]) if len(p) >= 51 else (0, 0, 0)
    return (f"flags=0x{flags:02X} sig_kbd={sig_kbd} age_kbd={age_k}ms pkt_rx_kbd={rx_k} pkt_dup_kbd={dup_k} "
            f"overwritten_transitions={overwritten} engine_gap_max={gap}ms "
            f"usb_ok={usb[0]} usb_refused={usb[1]} resumes={usb[2]} resumes_failed={usb[3]} "
            f"re_presses={repress} last=half{rh}:key{rk}:{rms}ms")


def main() -> None:
    if len(sys.argv) < 3 or sys.argv[2] not in KS_CMD:
        raise SystemExit(__doc__)
    port, what = sys.argv[1], sys.argv[2]
    if what == "version":
        print(request(port, KS_CMD[what]).decode(errors="replace"))
    elif what == "dfu":
        request(port, KS_CMD[what])
        print("rebooting into download mode — esptool --chip esp32s3 -p PORT --before no_reset --after hard_reset write_flash 0x20000 KeSp.bin")
    elif what == "rfstat":
        print(rfstat(request(port, KS_CMD[what])))
    elif what == "pair":
        reset = 1 if len(sys.argv) > 3 and sys.argv[3] == "1" else 0
        p = request(port, KS_CMD[what], bytes([reset]))
        print(f"pairing window open for 30 s — set_id=0x{p[0]:02X}{p[1]:02X} paired={p[2]}")
    elif what == "pairs":
        p = request(port, KS_CMD[what])
        print(p.hex(" "))


if __name__ == "__main__":
    main()
