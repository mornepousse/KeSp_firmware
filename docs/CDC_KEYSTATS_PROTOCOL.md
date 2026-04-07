# KeSp CDC Key Statistics Protocol

Protocol for reading key usage statistics from a KeSp-based keyboard via USB CDC (serial port).

## Connection

The keyboard exposes a CDC ACM serial port. Connect at any baud rate (USB CDC ignores baud rate settings). Send commands as text lines terminated by `\r\n`.

## Commands

### `KEYSTATS` — Binary key statistics

Send: `KEYSTATS\r\n`

Returns a binary packet containing per-key press counts, ordered in V2 layout (consistent across all hardware versions).

#### Response format

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | magic | `"KEYSTATS"` (ASCII, no null terminator) |
| 8 | 1 | rows | Number of matrix rows (5) |
| 9 | 1 | cols | Number of matrix columns (13) |
| 10 | 1 | hw_version | Hardware version: `0x01` = V1, `0x02` = V2/V2_Debug |
| 11 | 4 | total_presses | Total keypresses across all keys (uint32_t LE) |
| 15 | 4 | max_presses | Maximum press count on any single key (uint32_t LE) |
| 19 | rows×cols×4 | data | Press count per key (uint32_t LE each) |

**Total size**: 19 + 5×13×4 = **279 bytes**, followed by `\r\n` (2 bytes).

#### Data layout (V2 order)

Data is sent row by row, column by column, in V2 physical layout order:

```
Index = row * 13 + col

Row 0 (top):    DEL, 1, 2, 3, 4, 5, LBRC, 6, 7, 8, 9, 0, EQL
Row 1:          TAB, QUOT, COMM, DOT, P, Y, MO_L2, F, G, C, R, L, SLSH
Row 2:          RALT, A, O, E, U, I, RBRC, D, H, T, N, S, MINUS
Row 3:          LCTRL, SCLN, Q, J, K, X, LWIN, B, M, W, V, Z, GRV
Row 4 (bottom): ESC, ENTER, LALT, LWIN, LSHIFT, SPACE, -, BSPACE, ENTER, BSLSH, -, -, TO_L3
```

> Note: `-` entries in row 4 (cols 6, 10, 11) are unused positions (no physical key). Their count will always be 0.

#### Parsing example (Python)

```python
import serial
import struct

ser = serial.Serial('/dev/ttyACM0', timeout=2)
ser.write(b'KEYSTATS\r\n')

# Read header (19 bytes)
header = ser.read(19)
magic = header[0:8]
assert magic == b'KEYSTATS'

rows = header[8]
cols = header[9]
hw_version = header[10]
total = struct.unpack_from('<I', header, 11)[0]
max_val = struct.unpack_from('<I', header, 15)[0]

print(f"Hardware: V{hw_version}, Total: {total}, Max: {max_val}")
print(f"Matrix: {rows}x{cols}")

# Read data (rows * cols * 4 bytes)
data_size = rows * cols * 4
raw = ser.read(data_size)

# Parse into 2D array
stats = []
for r in range(rows):
    row = []
    for c in range(cols):
        offset = (r * cols + c) * 4
        count = struct.unpack_from('<I', raw, offset)[0]
        row.append(count)
    stats.append(row)

# Read end marker (\r\n)
ser.read(2)

# Display heatmap
for r in range(rows):
    print(f"R{r}: " + " ".join(f"{v:5d}" for v in stats[r]))
```

#### Heatmap visualization

To display a heatmap, normalize each count against `max_presses`:

```python
for r in range(rows):
    for c in range(cols):
        if max_val > 0:
            intensity = stats[r][c] / max_val  # 0.0 to 1.0
        else:
            intensity = 0.0
        # Map intensity to color (e.g., blue=cold -> red=hot)
```

### `KEYSTATS?` — Text key statistics

Send: `KEYSTATS?\r\n`

Returns human-readable text (for debugging):

```
Key Statistics - Total: 12345, Max: 500\r\n
R0:   100    50    30    20    10     5     0    15    25    35    45    55    65\r\n
R1:   ...
R2:   ...
R3:   ...
R4:   ...
OK\r\n
```

> Note: Text format uses internal (V1/V2) physical order, not translated. Use the binary `KEYSTATS` command for consistent V2-ordered data.

### `KEYSTATS_RESET` — Reset all statistics

Send: `KEYSTATS_RESET\r\n`

Response: `KEYSTATS_RESET:OK\r\n`

Clears all key press counters to zero and persists the reset to NVS flash.

---

## Bigram Commands

A bigram is a pair of consecutive keypresses (key A followed by key B). Useful for layout optimization.

Keys are identified by a flat index: `key_index = row * 13 + col` (range 0–64).

### `BIGRAMS` — Binary bigram statistics

Send: `BIGRAMS\r\n`

Returns the top 256 most frequent bigrams, sorted by count descending.

#### Response format

**Header (18 bytes):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | magic | `"BIGRAMS\0"` (7 chars + null byte) |
| 8 | 1 | hw_version | Hardware version (`0x01` = V1, `0x02` = V2) |
| 9 | 1 | num_keys | Total number of keys (65) |
| 10 | 4 | total | Total bigram events (uint32_t LE) |
| 14 | 2 | max_count | Max count on any single bigram (uint16_t LE) |
| 16 | 2 | n_entries | Number of entries following (uint16_t LE, max 256) |

**Entries (n_entries × 4 bytes each):**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | prev_key | First key index (row×13+col) |
| 1 | 1 | curr_key | Second key index (row×13+col) |
| 2 | 2 | count | Number of occurrences (uint16_t LE) |

Followed by `\r\n` end marker.

#### Converting key index to row/col

```python
row = key_index // 13
col = key_index % 13
```

#### Parsing example (Python)

```python
import serial
import struct

ser = serial.Serial('/dev/ttyACM0', timeout=2)
ser.write(b'BIGRAMS\r\n')

# Read header (18 bytes)
header = ser.read(18)
magic = header[0:7]
assert magic == b'BIGRAMS'

hw_version = header[8]
num_keys = header[9]
total = struct.unpack_from('<I', header, 10)[0]
max_count = struct.unpack_from('<H', header, 14)[0]
n_entries = struct.unpack_from('<H', header, 16)[0]

print(f"Total bigrams: {total}, Max: {max_count}, Entries: {n_entries}")

# Read entries
bigrams = []
for _ in range(n_entries):
    entry = ser.read(4)
    prev_key = entry[0]
    curr_key = entry[1]
    count = struct.unpack_from('<H', entry, 2)[0]
    bigrams.append((prev_key, curr_key, count))

# Read end marker
ser.read(2)

# Display top bigrams with row/col
COLS = 13
for prev, curr, count in bigrams[:20]:
    pr, pc = prev // COLS, prev % COLS
    cr, cc = curr // COLS, curr % COLS
    print(f"  R{pr}C{pc} -> R{cr}C{cc} : {count}")
```

#### Bigram heatmap (chord visualization)

To build a 2D bigram heatmap (65×65 matrix):

```python
import numpy as np

heatmap = np.zeros((num_keys, num_keys), dtype=int)
for prev, curr, count in bigrams:
    heatmap[prev][curr] = count

# Normalize for visualization
if max_count > 0:
    heatmap_norm = heatmap / max_count
```

### `BIGRAMS?` — Text bigram statistics

Send: `BIGRAMS?\r\n`

Returns the top 20 bigrams in human-readable format:

```
Bigram Statistics - Total: 5000, Max: 150\r\n
  R2C3 -> R2C4 : 150\r\n
  R2C4 -> R2C3 : 120\r\n
  R1C4 -> R2C3 : 95\r\n
  ...\r\n
OK\r\n
```

### `BIGRAMS_RESET` — Reset all bigram statistics

Send: `BIGRAMS_RESET\r\n`

Response: `BIGRAMS_RESET:OK\r\n`

---

## Layout Commands

### `LAYOUT?` — Physical key layout JSON

Send: `LAYOUT?\r\n`

Returns a JSON string describing the physical key positions. Use binary command `KS_CMD_GET_LAYOUT_JSON` (0x22) — response is streamed in 256-byte chunks.

The JSON contains:
- `name`: product name
- `rows`, `cols`: matrix dimensions
- `keys`: total number of physical keys (64)
- `groups`: array of key groups (left hand, right hand)

Each group contains `lines` (vertical columns), each line contains `keycaps` (individual keys). Keys are described with:
- `row`, `col`: matrix coordinates
- `margin`: CSS-like pixel margins `"left,top,right,bottom"`
- `angle`: rotation in degrees (positive = clockwise)
- `width`: key width in pixels (omitted = default 1u)

> The layout is board-independent (all KaSe boards share the same physical layout). Column 6, row 1 has no physical key.

### `LAYOUTS?` — List layout names

Send: `LAYOUTS?\r\n`

Returns one line per layer with format: `<index>:<name>\r\n`, followed by `OK\r\n`.

### `LAYOUTNAME<idx>:<name>` — Rename a layer

Send: `LAYOUTNAME0:AZERTY_FR\r\n`

Renames layer `<idx>` (0-9) to `<name>` (max 14 chars) and persists to NVS.

Response: `LAYOUTNAME:OK\r\n`

---

## OTA Firmware Update

Flash new firmware over USB CDC without esptool. Keymaps, macros and statistics in NVS are preserved.

### `OTA <size>` — Start OTA update

Send: `OTA 971152\r\n` (firmware binary size in bytes)

Response:
- `OTA_READY 4096\r\n` — device is ready, send data in 4096-byte chunks
- `OTA_ERROR <reason>\r\n` — cannot start (no partition, invalid size, etc.)

### Binary data transfer

After `OTA_READY`, switch to binary mode: send raw firmware bytes in chunks of 4096 (last chunk may be smaller).

**Wait for ACK after each chunk:**
- `OTA_OK <received>/<total>\r\n` — chunk written, send next
- `OTA_FAIL <reason>\r\n` — write error, OTA aborted

### Completion

After the last chunk is written and validated:
- `OTA_DONE\r\n` — success, device reboots into new firmware
- `OTA_FAIL validation failed\r\n` — image invalid, device stays on current firmware

### Timeout

If no data is received for 30 seconds during transfer, the OTA is aborted automatically.

### Protocol flow

```
Host                              Device
  │                                  │
  │  "OTA 971152\r\n"               │
  │ ──────────────────────────────>  │
  │                                  │  (erase OTA partition)
  │           "OTA_READY 4096\r\n"   │
  │ <──────────────────────────────  │
  │                                  │
  │  <4096 bytes raw binary>         │
  │ ──────────────────────────────>  │
  │                                  │  (write to flash)
  │      "OTA_OK 4096/971152\r\n"    │
  │ <──────────────────────────────  │
  │                                  │
  │  <4096 bytes raw binary>         │
  │ ──────────────────────────────>  │
  │      "OTA_OK 8192/971152\r\n"    │
  │ <──────────────────────────────  │
  │                                  │
  │  ... (repeat until all sent) ... │
  │                                  │
  │         "OTA_DONE\r\n"           │
  │ <──────────────────────────────  │
  │                                  │  (reboot)
```

### Example (Python)

```python
import serial
import struct
import os

firmware_path = "KeSp_v3.4_V2_Debug.bin"
firmware_size = os.path.getsize(firmware_path)

ser = serial.Serial('/dev/ttyACM0', timeout=5)

# Start OTA
ser.write(f"OTA {firmware_size}\r\n".encode())
response = ser.readline().decode().strip()
if not response.startswith("OTA_READY"):
    raise RuntimeError(f"OTA start failed: {response}")

chunk_size = int(response.split()[1])

# Send firmware
with open(firmware_path, "rb") as f:
    sent = 0
    while sent < firmware_size:
        chunk = f.read(chunk_size)
        ser.write(chunk)
        sent += len(chunk)

        ack = ser.readline().decode().strip()
        if ack.startswith("OTA_OK"):
            progress = sent * 100 // firmware_size
            print(f"\r{progress}%", end="", flush=True)
        elif ack.startswith("OTA_DONE"):
            print("\nOTA complete! Device rebooting...")
            break
        else:
            raise RuntimeError(f"\nOTA failed: {ack}")
```

### Partition layout

The firmware uses a dual-partition scheme:

| Partition | Offset | Size | Purpose |
|-----------|--------|------|---------|
| factory | 0x20000 | 2MB | Recovery firmware (flashed via esptool) |
| ota_0 | 0x220000 | 2MB | OTA target (updated via CDC) |

NVS (keymaps, macros, stats) is at 0x9000 and is never touched by OTA.

### First-time setup

The first flash with OTA support requires esptool (full partition table change):

```bash
esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 \
  write_flash 0x0 bootloader.bin 0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin 0x20000 KeSp.bin 0x420000 storage.bin
```

After this initial flash, all future updates can use the CDC OTA protocol.

---

## Advanced Features Commands

See [KEYCODE_MAP.md](KEYCODE_MAP.md) for keycode encoding details.

### `FEATURES?` — Query supported features

Send: `FEATURES?\r\n`

Response: `MT,LT,OSM,OSL,CAPS_WORD,REPEAT,TAP_DANCE,COMBO,LEADER\r\n`

### `SETKEY layer,v2row,v2col,hex_keycode` — Set a single key

Send: `SETKEY 0,4,0,5229\r\n` (Layer 0, V2 row 4 col 0 = MT(Shift, ESC))

### `TDSET index;a1,a2,a3,a4` — Configure Tap Dance

All values in **hex**. Actions: 1-tap, 2-tap, 3-tap, hold.

Send: `TDSET 0;04,05,06,29\r\n` (1-tap=A, 2-tap=B, 3-tap=C, hold=ESC)

Response: `TDSET 0:OK\r\n`

### `TD?` — List Tap Dance configs

Response: one line per configured slot, then `OK`.

### `COMBOSET index;r1,c1,r2,c2,result_hex` — Configure Combo

Positions use **V1 internal coordinates**.

Send: `COMBOSET 0;3,3,3,4,29\r\n` (J+K = ESC)

Response: `COMBOSET 0:OK\r\n`

### `COMBO?` — List Combo configs

### `LEADERSET index;seq_hex;result_hex,mod_hex` — Configure Leader sequence

Sequence is comma-separated HID keycodes in hex. Result is keycode + modifier mask.

Send: `LEADERSET 0;09,16;16,01\r\n` (Leader → F → S = Ctrl+S)

Response: `LEADERSET 0:OK\r\n`

### `LEADER?` — List Leader sequences

### `MACROSEQ slot;name;key:mod,...` — Create sequence macro

Each step is `keycode:modifier` in hex. Special: `FF:nn` = delay (nn × 10ms).

Send: `MACROSEQ 0;CopyPaste;06:01,FF:0A,19:01\r\n` (Ctrl+C, 100ms, Ctrl+V)

Response: `MACRO 0 saved (3 steps)\r\n`

### `MACROADD slot;name;key1,key2,...` — Create simple macro (simultaneous keys)

Send: `MACROADD 0;Copy;E0,06\r\n` (Ctrl+C)

### `MACRODEL slot` — Delete macro

Send: `MACRODEL 0\r\n`

---

## Data persistence

- Key stats and bigram stats are stored in NVS flash and survive reboots
- Key stats auto-save: every **100 keypresses** or every **60 seconds**
- Bigram stats auto-save: every **100 keypresses** or every **120 seconds**
- Tap dance, combo, and leader configs persist to NVS on set
- Macro sequences persist to NVS on save
- On boot, all configs are loaded from NVS automatically
