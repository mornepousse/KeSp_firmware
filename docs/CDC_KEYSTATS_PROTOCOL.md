# KaSe CDC Key Statistics Protocol

Protocol for reading key usage statistics from the KaSe keyboard via USB CDC (serial port).

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

## Data persistence

- Key stats and bigram stats are stored in NVS flash and survive reboots
- Key stats auto-save: every **100 keypresses** or every **60 seconds**
- Bigram stats auto-save: every **100 keypresses** or every **120 seconds**
- On boot, both are loaded from NVS automatically
