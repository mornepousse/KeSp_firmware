#!/usr/bin/env python3
"""KaSe binary CDC protocol test suite.

Requires the keyboard to be connected via USB CDC.
Uses the ESP-IDF venv for pyserial: source esp-idf/export.sh first.

Usage:
    python3 scripts/test_binary_protocol.py [/dev/ttyACM0]
"""
import sys
import time
import struct
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
BAUD = 115200
TIMEOUT = 2

# ── Protocol helpers ─────────────────────────────────────────────

def crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def ks_frame(cmd_id: int, payload: bytes = b"") -> bytes:
    hdr = bytes([0x4B, 0x53, cmd_id, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    return hdr + payload + bytes([crc8(payload)])

def parse_kr(resp: bytes):
    if len(resp) < 7 or resp[0] != 0x4B or resp[1] != 0x52:
        return None
    cmd = resp[2]
    status = resp[3]
    plen = resp[4] | (resp[5] << 8)
    if len(resp) < 6 + plen + 1:
        return None
    payload = resp[6:6 + plen]
    crc_received = resp[6 + plen]
    crc_expected = crc8(payload)
    return {
        "cmd": cmd,
        "status": status,
        "len": plen,
        "payload": payload,
        "crc_ok": crc_received == crc_expected,
    }

# ── Test runner ──────────────────────────────────────────────────

class TestRunner:
    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=TIMEOUT)
        time.sleep(1)
        self.ser.reset_input_buffer()
        self.passed = 0
        self.failed = 0

    def close(self):
        self.ser.close()

    def send(self, cmd_id, payload=b"", read_size=8192, delay=0.3):
        self.ser.reset_input_buffer()
        self.ser.write(ks_frame(cmd_id, payload))
        time.sleep(delay)
        data = self.ser.read(read_size)
        # For large responses, keep reading until we have a complete KR frame
        if len(data) >= 6:
            plen = data[4] | (data[5] << 8)
            expected = 6 + plen + 1  # header + payload + crc
            retries = 0
            while len(data) < expected and retries < 10:
                more = self.ser.read(expected - len(data))
                if not more:
                    break
                data += more
                retries += 1
        return data

    def expect(self, name, cmd_id, payload=b"", status=0, min_len=0, max_len=None,
               check_payload=None, delay=0.3):
        resp = self.send(cmd_id, payload, delay=delay)
        r = parse_kr(resp)

        errors = []
        if r is None:
            errors.append(f"no valid KR response ({len(resp)} raw bytes)")
        else:
            if r["status"] != status:
                errors.append(f"status={r['status']} expected={status}")
            if not r["crc_ok"]:
                errors.append("CRC mismatch")
            if r["len"] < min_len:
                errors.append(f"len={r['len']} < min={min_len}")
            if max_len is not None and r["len"] > max_len:
                errors.append(f"len={r['len']} > max={max_len}")
            if check_payload and not check_payload(r["payload"]):
                errors.append(f"payload check failed: {r['payload'][:20].hex()}")

        if errors:
            self.failed += 1
            print(f"  FAIL  {name}: {', '.join(errors)}")
        else:
            self.passed += 1
            detail = ""
            if r and r["len"] > 0 and r["len"] <= 80:
                try:
                    text = r["payload"].decode("ascii")
                    if all(32 <= c < 127 for c in r["payload"]):
                        detail = f' "{text}"'
                except:
                    pass
            if not detail and r:
                detail = f" ({r['len']}B)"
            print(f"  OK    {name}{detail}")
        return r

    def send_legacy(self, text):
        self.ser.reset_input_buffer()
        self.ser.write((text + "\n").encode())
        time.sleep(0.3)
        return self.ser.read(512).decode(errors="replace").strip()

# ── Tests ────────────────────────────────────────────────────────

def test_system(t: TestRunner):
    print("\n=== System ===")
    t.expect("PING", 0x04)
    t.expect("VERSION", 0x01, min_len=5,
             check_payload=lambda p: p[:2] == b"v3")
    t.expect("FEATURES", 0x02, min_len=10,
             check_payload=lambda p: b"MT" in p and b"LT" in p)

def test_keymap(t: TestRunner):
    print("\n=== Keymap ===")
    r = t.expect("LAYER_INDEX", 0x14, min_len=1, max_len=1)
    layer = r["payload"][0] if r else 0

    t.expect("KEYMAP_CURRENT", 0x12, min_len=10,
             check_payload=lambda p: p[0] == layer)
    t.expect("KEYMAP_GET layer0", 0x13, payload=b"\x00", min_len=10,
             check_payload=lambda p: p[0] == 0)
    t.expect("KEYMAP_GET invalid", 0x13, payload=b"\xFF", status=4)  # ERR_RANGE

    t.expect("LAYER_NAME layer0", 0x15, payload=b"\x00", min_len=2,
             check_payload=lambda p: p[0] == 0)
    t.expect("LAYER_NAME invalid", 0x15, payload=b"\xFF", status=4)

def test_layout(t: TestRunner):
    print("\n=== Layout ===")
    t.expect("LIST_LAYOUTS", 0x21, min_len=3)
    t.expect("GET_LAYOUT_JSON", 0x22, min_len=10, delay=2.0,
             check_payload=lambda p: p[0:1] == b"{")

def test_macros(t: TestRunner):
    print("\n=== Macros ===")
    t.expect("LIST_MACROS", 0x30, min_len=1)
    # Don't modify macros in tests — just verify queries work

def test_stats(t: TestRunner):
    print("\n=== Stats ===")
    t.expect("KEYSTATS_BIN", 0x40, min_len=3)
    t.expect("BIGRAMS_BIN", 0x43, min_len=8)

def test_tap_dance(t: TestRunner):
    print("\n=== Tap Dance ===")
    t.expect("TD_LIST", 0x51, min_len=1)

def test_combos(t: TestRunner):
    print("\n=== Combos ===")
    t.expect("COMBO_LIST", 0x61, min_len=1)

def test_leader(t: TestRunner):
    print("\n=== Leader ===")
    t.expect("LEADER_LIST", 0x71, min_len=1)

def test_ko(t: TestRunner):
    print("\n=== Key Override ===")
    t.expect("KO_LIST", 0x92, min_len=1)

def test_bluetooth(t: TestRunner):
    print("\n=== Bluetooth ===")
    r = t.expect("BT_QUERY", 0x80, min_len=4)
    if r and r["payload"]:
        slot, init, conn, pairing = r["payload"][:4]
        print(f"         slot={slot} init={init} conn={conn} pairing={pairing}")

def test_tama(t: TestRunner):
    print("\n=== Tamagotchi ===")
    r = t.expect("TAMA_QUERY", 0xA0, min_len=20)
    if r and r["payload"]:
        enabled = r["payload"][0]
        state = r["payload"][1]
        hunger = struct.unpack_from("<H", r["payload"], 2)[0]
        happy = struct.unpack_from("<H", r["payload"], 4)[0]
        energy = struct.unpack_from("<H", r["payload"], 6)[0]
        print(f"         enabled={enabled} state={state} hunger={hunger} happy={happy} energy={energy}")

def test_features(t: TestRunner):
    print("\n=== Features ===")
    t.expect("WPM", 0x93, min_len=2, max_len=2)
    t.expect("AUTOSHIFT_TOGGLE", 0x90, min_len=1, max_len=1)
    # Toggle back
    t.expect("AUTOSHIFT_TOGGLE (restore)", 0x90, min_len=1, max_len=1)

def test_ota_cycle(t: TestRunner):
    print("\n=== OTA (start/abort cycle) ===")
    t.expect("OTA_START 100KB", 0xF0,
             payload=struct.pack("<I", 100000),
             min_len=2,
             check_payload=lambda p: struct.unpack("<H", p)[0] == 4096)
    t.expect("OTA_ABORT", 0xF2)
    t.expect("PING (post-abort)", 0x04)

def test_ota_bad_size(t: TestRunner):
    print("\n=== OTA (bad size) ===")
    t.expect("OTA_START 0 bytes", 0xF0,
             payload=struct.pack("<I", 0), status=4)  # ERR_RANGE
    t.expect("OTA_START too big", 0xF0,
             payload=struct.pack("<I", 0x300000), status=4)

def test_crc_error(t: TestRunner):
    print("\n=== CRC error handling ===")
    # Send a PING frame with wrong CRC
    frame = bytes([0x4B, 0x53, 0x04, 0x00, 0x00, 0xFF])  # CRC should be 0x00 not 0xFF
    t.ser.reset_input_buffer()
    t.ser.write(frame)
    time.sleep(0.3)
    resp = t.ser.read(64)
    r = parse_kr(resp)
    if r and r["status"] == 2:  # ERR_CRC
        t.passed += 1
        print("  OK    CRC error detected → status=ERR_CRC")
    elif r:
        t.failed += 1
        print(f"  FAIL  CRC error: status={r['status']} expected=2")
    else:
        t.failed += 1
        print(f"  FAIL  CRC error: no valid response ({len(resp)} bytes)")

def test_unknown_cmd(t: TestRunner):
    print("\n=== Unknown command ===")
    t.expect("Unknown cmd 0xFE", 0xFE, status=1)  # ERR_UNKNOWN

def test_legacy_coexistence(t: TestRunner):
    print("\n=== Legacy ASCII coexistence ===")
    resp = t.send_legacy("VERSION?")
    if "v3" in resp.lower() or "kase" in resp.lower():
        t.passed += 1
        print(f'  OK    Legacy VERSION? → "{resp}"')
    else:
        t.failed += 1
        print(f'  FAIL  Legacy VERSION? → "{resp}"')

    # Binary after legacy — small delay to let serial settle
    time.sleep(0.5)
    t.expect("PING (after legacy)", 0x04)

# ── Main ─────────────────────────────────────────────────────────

def main():
    print(f"KaSe Binary Protocol Test Suite")
    print(f"Port: {PORT}")

    t = TestRunner(PORT)

    test_system(t)
    test_keymap(t)
    test_layout(t)
    test_macros(t)
    test_stats(t)
    test_tap_dance(t)
    test_combos(t)
    test_leader(t)
    test_ko(t)
    test_bluetooth(t)
    test_tama(t)
    test_features(t)
    test_ota_cycle(t)
    test_ota_bad_size(t)
    test_crc_error(t)
    test_unknown_cmd(t)
    test_legacy_coexistence(t)

    t.close()

    print(f"\n{'='*40}")
    total = t.passed + t.failed
    print(f"Results: {t.passed}/{total} passed, {t.failed} failed")
    if t.failed == 0:
        print("ALL TESTS PASSED")
    sys.exit(0 if t.failed == 0 else 1)

if __name__ == "__main__":
    main()
