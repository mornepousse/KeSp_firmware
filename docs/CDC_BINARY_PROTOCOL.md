# KaSe Binary CDC Protocol

Protocol binaire pour la communication entre KaSe_soft et le firmware. Coexiste avec le protocole ASCII legacy — le firmware auto-detecte le mode par le premier octet.

## Frame Format

### Request (Host → Keyboard)

```
Offset  Size  Field
  0       1    Magic 0x4B ('K')
  1       1    Magic 0x53 ('S')
  2       1    Command ID
  3       2    Payload length (u16 LE)
  5       N    Payload (0 to 4096 bytes)
  5+N     1    CRC-8 of payload
```

### Response (Keyboard → Host)

```
Offset  Size  Field
  0       1    Magic 0x4B ('K')
  1       1    Magic 0x52 ('R')
  2       1    Command ID (echo)
  3       1    Status code
  4       2    Payload length (u16 LE)
  6       N    Payload
  6+N     1    CRC-8 of payload
```

## CRC-8

Polynomial: **0x31** (CRC-8/MAXIM), init 0x00, no reflection.
Computed over payload bytes only (not the header).
Empty payload → CRC = 0x00.

```c
uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}
```

```python
def crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc
```

## Status Codes

| Code | Name | Description |
|------|------|-------------|
| 0x00 | OK | Success |
| 0x01 | ERR_UNKNOWN | Unknown command ID |
| 0x02 | ERR_CRC | CRC mismatch |
| 0x03 | ERR_INVALID | Invalid payload format |
| 0x04 | ERR_RANGE | Parameter out of range |
| 0x05 | ERR_BUSY | Resource busy (OTA in progress) |
| 0x06 | ERR_OVERFLOW | Payload exceeds max size |

## Backward Compatibility

Si le premier octet recu n'est pas `0x4B`, le firmware bascule en mode texte ASCII legacy. Aucune commande texte ne commence par `KS`, donc pas d'ambiguïte.

L'ancienne version de KaSe_soft continue de fonctionner pour l'OTA et toutes les commandes texte.

---

## Command Reference

### System (0x01–0x0F)

#### PING (0x04)
Teste la connexion.
- Request: `KS 04 0000 00`
- Response: `KR 04 00 0000 00`

#### VERSION (0x01)
Retourne la version firmware.
- Request: payload vide
- Response: payload = version string UTF-8 (ex: `v3.2-87-gd34c39f`)

#### FEATURES (0x02)
Liste des features supportees.
- Request: payload vide
- Response: payload = liste CSV (ex: `MT,LT,OSM,OSL,CAPS_WORD,...`)

#### DFU (0x03)
Reboot en mode DFU. Reponse OK envoyee avant reboot.
- Request: payload vide
- Response: OK puis reboot

---

### Keymap (0x10–0x1F)

#### LAYER_INDEX (0x14)
Retourne le layer actif.
- Request: payload vide
- Response: `[layer:u8]`

#### KEYMAP_CURRENT (0x12)
Keymap du layer actif.
- Request: payload vide
- Response: `[layer:u8][keycodes: ROWS*COLS * u16 LE]`

#### KEYMAP_GET (0x13)
Keymap d'un layer specifique.
- Request: `[layer:u8]`
- Response: `[layer:u8][keycodes: ROWS*COLS * u16 LE]`
- Erreur: `ERR_RANGE` si layer >= LAYERS

#### SETKEY (0x11)
Modifie une touche. Sauvegarde immediate en NVS.
- Request: `[layer:u8][row:u8][col:u8][value:u16 LE]`
- Response: OK
- Note: coordonnees natives du board (chaque variante a son propre layout)

#### SETLAYER (0x10)
Remplace un layer entier. Sauvegarde immediate.
- Request: `[layer:u8][keycodes: ROWS*COLS * u16 LE]`
- Response: OK

#### LAYER_NAME (0x15)
Nom d'un layer.
- Request: `[layer:u8]`
- Response: `[layer:u8][name bytes]`

---

### Layout (0x20–0x2F)

#### LIST_LAYOUTS (0x21)
Tous les noms de layers.
- Request: payload vide
- Response: `[count:u8][{idx:u8, name_len:u8, name[]}...]`

#### SET_LAYOUT_NAME (0x20)
Renomme un layer. Sauvegarde immediate.
- Request: `[layer:u8][name bytes]`
- Response: OK

#### GET_LAYOUT_JSON (0x22)
JSON de la disposition physique du clavier.
- Request: payload vide
- Response: payload = JSON brut (peut depasser 4KB, envoye en streaming)

---

### Macros (0x30–0x3F)

#### LIST_MACROS (0x30)
Liste toutes les macros configurees.
- Request: payload vide
- Response:
```
[count:u8]
{
  [idx:u8]
  [keycode:u16 LE]       — keycode macro (MACRO_1 + idx*0x100)
  [name_len:u8][name[]]
  [keys_len:u8][keys[]]  — legacy simultaneous keys
  [step_count:u8]         — sequence steps
  [{kc:u8, mod:u8}...]   — step_count * 2 bytes
}...
```

#### MACRO_ADD (0x31)
Ajoute une macro legacy (touches simultanees).
- Request: `[slot:u8][name_len:u8][name...][keys: 6 bytes]`
- Response: OK

#### MACRO_ADD_SEQ (0x32)
Ajoute une macro sequence.
- Request: `[slot:u8][name_len:u8][name...][step_count:u8][{kc:u8,mod:u8}...]`
- Response: OK
- Note: `kc=0xFF` + `mod=N` = delay de N*10ms

#### MACRO_DELETE (0x33)
Supprime une macro.
- Request: `[slot:u8]`
- Response: OK

---

### Statistics (0x40–0x4F)

#### KEYSTATS_BIN (0x40)
Compteurs de touches par position (format binaire structure).
- Request: payload vide
- Response: `[rows:u8][cols:u8][counts: rows*cols * u32 LE]`
- Note: coordonnees V2

#### KEYSTATS_TEXT (0x41)
Compteurs de touches par position (format texte lisible).
- Request: payload vide
- Response: payload = texte UTF-8 multi-lignes
```
Key Statistics - Total: 12345, Max: 678
R0:   123   456   789 ...
R1:    42    99   301 ...
...
```

#### KEYSTATS_RESET (0x42)
Remet les compteurs a zero.
- Request: payload vide
- Response: OK

#### BIGRAMS_BIN (0x43)
Top 256 bigrammes tries par frequence (format binaire structure).
- Request: payload vide
- Response:
```
[module_id:u8]
[num_keys:u8]
[total:u32 LE]
[max:u16 LE]
[{prev:u8, curr:u8, count:u16 LE}...]  — top entries sorted desc
```

#### BIGRAMS_TEXT (0x44)
Top 20 bigrammes (format texte lisible).
- Request: payload vide
- Response: payload = texte UTF-8 multi-lignes
```
Bigram Statistics - Total: 5678, Max: 42
  R1C3 -> R0C5 : 42
  R0C2 -> R0C3 : 38
  ...
```

#### BIGRAMS_RESET (0x45)
Remet les bigrammes a zero.
- Request: payload vide
- Response: OK

---

### Tap Dance (0x50–0x5F)

#### TD_LIST (0x51)
Liste les tap dances configures.
- Request: payload vide
- Response: `[count:u8][{idx:u8, a1:u8, a2:u8, a3:u8, a4:u8}...]`
- a1=1-tap, a2=2-taps, a3=3-taps, a4=hold (HID keycodes)

#### TD_SET (0x50)
Configure un tap dance.
- Request: `[index:u8][a1:u8][a2:u8][a3:u8][a4:u8]`
- Response: OK

#### TD_DELETE (0x52)
Supprime un tap dance.
- Request: `[index:u8]`
- Response: OK

---

### Combos (0x60–0x6F)

#### COMBO_LIST (0x61)
Liste les combos configures.
- Request: payload vide
- Response: `[count:u8][{idx:u8, row1:u8, col1:u8, row2:u8, col2:u8, result:u8}...]`

#### COMBO_SET (0x60)
Configure un combo.
- Request: `[index:u8][row1:u8][col1:u8][row2:u8][col2:u8][result:u8]`
- Response: OK
- Note: positions en coordonnees V1 internes

#### COMBO_DELETE (0x62)
Supprime un combo.
- Request: `[index:u8]`
- Response: OK

---

### Leader Key (0x70–0x7F)

#### LEADER_LIST (0x71)
Liste les sequences leader.
- Request: payload vide
- Response: `[count:u8][{idx:u8, seq_len:u8, seq[], result:u8, result_mod:u8}...]`

#### LEADER_SET (0x70)
Configure une sequence leader.
- Request: `[index:u8][seq_len:u8][seq...][result:u8][result_mod:u8]`
- Response: OK
- La sequence est une suite de HID keycodes (max 4), result_mod = modifier mask

#### LEADER_DELETE (0x72)
Supprime une sequence leader.
- Request: `[index:u8]`
- Response: OK

---

### Bluetooth (0x80–0x8F)

#### BT_QUERY (0x80)
Etat complet du Bluetooth.
- Request: payload vide
- Response:
```
[active_slot:u8]
[initialized:u8]   — 0/1
[connected:u8]      — 0/1
[pairing:u8]        — 0/1
[{slot_idx:u8, valid:u8, addr[6], name_len:u8, name[]}...]  — BT_MAX_DEVICES (3) entries
```

#### BT_SWITCH (0x81)
Change de slot BT.
- Request: `[slot:u8]` (0-2)
- Response: OK (note: la reconnexion peut prendre ~3s)

#### BT_PAIR (0x82)
Active le mode pairing (advertising non-dirige).
- Request: payload vide
- Response: OK

#### BT_DISCONNECT (0x83)
Deconnecte l'appareil actuel.
- Request: payload vide
- Response: OK

#### BT_NEXT (0x84) / BT_PREV (0x85)
Slot suivant/precedent.
- Request: payload vide
- Response: OK

---

### Features (0x90–0x9F)

#### AUTOSHIFT_TOGGLE (0x90)
Bascule l'auto-shift on/off.
- Request: payload vide
- Response: `[enabled:u8]` (0 ou 1, etat apres toggle)

#### KO_SET (0x91)
Configure un key override.
- Request: `[index:u8][trigger_key:u8][trigger_mod:u8][result_key:u8][result_mod:u8]`
- Response: OK

#### KO_LIST (0x92)
Liste les key overrides.
- Request: payload vide
- Response: `[count:u8][{idx:u8, trigger_key:u8, trigger_mod:u8, result_key:u8, result_mod:u8}...]`

#### KO_DELETE (0x93)
Supprime un key override.
- Request: `[index:u8]`
- Response: OK

#### WPM_QUERY (0x94)
Mots par minute actuels.
- Request: payload vide
- Response: `[wpm:u16 LE]`

#### TRILAYER_SET (0x95)
Configure le tri-layer.
- Request: `[layer1:u8][layer2:u8][result:u8]`
- Response: OK

---

### Tamagotchi (0xA0–0xAF)

#### TAMA_QUERY (0xA0)
Etat complet du tamagotchi.
- Request: payload vide
- Response:
```
[enabled:u8]
[state:u8]         — 0=idle, 1=happy, 2=excited, 3=eating, 4=sleepy, 5=sleeping, 6=sick, 7=sad, 8=celebrating
[hunger:u16 LE]    — 0-1000
[happiness:u16 LE] — 0-1000
[energy:u16 LE]    — 0-1000
[health:u16 LE]    — 0-1000 (computed average)
[level:u16 LE]     — 0-19
[xp:u16 LE]
[total_keys:u32 LE]
[max_kpm:u32 LE]
```

#### TAMA_ENABLE (0xA1) / TAMA_DISABLE (0xA2)
Active/desactive le tamagotchi.
- Request: payload vide
- Response: OK

#### TAMA_FEED (0xA3) / TAMA_PLAY (0xA4) / TAMA_SLEEP (0xA5) / TAMA_MEDICINE (0xA6)
Actions directes sur le pet.
- Request: payload vide
- Response: OK

#### TAMA_SAVE (0xA7)
Force la sauvegarde en NVS.
- Request: payload vide
- Response: OK

---

### OTA (0xF0–0xFF)

#### OTA_START (0xF0)
Demarre une mise a jour firmware.
- Request: `[firmware_size:u32 LE]`
- Response: `[chunk_size:u16 LE]` (toujours 4096)
- Erreur: `ERR_RANGE` si taille = 0 ou > 2MB

#### OTA_DATA (0xF1)
Envoie un chunk de firmware. Repeter jusqu'a completion.
- Request: payload = raw firmware bytes (max chunk_size)
- Response: `[received:u32 LE][total:u32 LE]`
- Quand received == total: firmware valide, reponse OK puis reboot
- Chaque chunk est CRC-protege par la frame KS

#### OTA_ABORT (0xF2)
Annule l'OTA en cours.
- Request: payload vide
- Response: OK
- Erreur: `ERR_INVALID` si aucun OTA en cours

### OTA Flow

```
Host                          Firmware
 │                               │
 │  KS [F0] [size:u32]          │
 ├──────────────────────────────>│
 │  KR [F0] OK [chunk_size:u16] │
 │<──────────────────────────────┤
 │                               │
 │  KS [F1] [chunk 1]           │   ─┐
 ├──────────────────────────────>│    │
 │  KR [F1] OK [recv][total]    │    │ repeat
 │<──────────────────────────────┤    │
 │  ...                          │   ─┘
 │                               │
 │  KS [F1] [last chunk]        │
 ├──────────────────────────────>│
 │  KR [F1] OK [total][total]   │  ← firmware valide
 │<──────────────────────────────┤
 │                               │  reboot
```

---

## Exemple Python complet

```python
import serial, struct

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def ks_frame(cmd_id, payload=b""):
    hdr = bytes([0x4B, 0x53, cmd_id, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    return hdr + payload + bytes([crc8(payload)])

def parse_kr(data):
    if len(data) < 7 or data[0:2] != b"\x4b\x52":
        return None
    cmd, status = data[2], data[3]
    plen = data[4] | (data[5] << 8)
    payload = data[6:6+plen]
    return {"cmd": cmd, "status": status, "payload": payload}

ser = serial.Serial("/dev/ttyACM0", timeout=2)

# Ping
ser.write(ks_frame(0x04))
r = parse_kr(ser.read(64))
assert r["status"] == 0

# Get version
ser.write(ks_frame(0x01))
r = parse_kr(ser.read(256))
print(f"Version: {r['payload'].decode()}")

# Get current layer keymap
ser.write(ks_frame(0x12))
r = parse_kr(ser.read(4096))
layer = r["payload"][0]
keycodes = [struct.unpack_from("<H", r["payload"], 1+i*2)[0] for i in range(65)]
print(f"Layer {layer}: {keycodes[:5]}...")

# Set one key: layer 0, row 0, col 0 = KC_A (0x04)
ser.write(ks_frame(0x11, bytes([0, 0, 0, 0x04, 0x00])))
r = parse_kr(ser.read(64))
assert r["status"] == 0

# BT query
ser.write(ks_frame(0x80))
r = parse_kr(ser.read(256))
slot, init, conn, pairing = r["payload"][:4]
print(f"BT: slot={slot} init={init} conn={conn} pairing={pairing}")

ser.close()
```

## Exemple C# (KaSe_soft)

```csharp
byte[] KsFrame(byte cmdId, byte[] payload = null) {
    payload ??= Array.Empty<byte>();
    var frame = new byte[5 + payload.Length + 1];
    frame[0] = 0x4B; frame[1] = 0x53;
    frame[2] = cmdId;
    frame[3] = (byte)(payload.Length & 0xFF);
    frame[4] = (byte)((payload.Length >> 8) & 0xFF);
    Array.Copy(payload, 0, frame, 5, payload.Length);
    frame[^1] = Crc8(payload);
    return frame;
}

byte Crc8(byte[] data) {
    byte crc = 0;
    foreach (var b in data) {
        crc ^= b;
        for (int i = 0; i < 8; i++)
            crc = (byte)((crc & 0x80) != 0 ? (crc << 1) ^ 0x31 : crc << 1);
    }
    return crc;
}
```

## Test

```bash
source ~/esp/esp-idf/export.sh
python3 scripts/test_binary_protocol.py /dev/ttyACM0
```

32 tests couvrant toutes les commandes, gestion d'erreurs, et coexistence legacy.
