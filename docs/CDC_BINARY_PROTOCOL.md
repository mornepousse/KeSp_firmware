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
- Response: payload = liste CSV (ex: `MT,LT,LM,OSM,OSL,CAPS_WORD,...`)

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

### Diagnostics (0xB0–0xBF)

#### MATRIX_TEST (0xB0)
Toggle le mode test matrice. En mode test, le clavier arrete d'envoyer les rapports HID et envoie a la place des evenements de changement d'etat des touches via des frames KR non-sollicitees.

- Request: payload vide (toggle on/off)
- Response: `[enabled:u8][rows:u8][cols:u8]`
  - `enabled`: 1 = mode test actif, 0 = mode normal
  - `rows`, `cols`: dimensions de la matrice

**Evenements (firmware → host, non-sollicites) :**

Quand le mode test est actif, chaque changement d'etat d'une touche genere une frame :
```
KR [0xB0] [OK] [3 bytes] [row:u8][col:u8][state:u8] [crc]
```
- `row`, `col`: position dans la matrice
- `state`: 1 = presse, 0 = relache

**Flux typique :**
```
Host                          Firmware
 │                               │
 │  KS [B0] (toggle ON)         │
 ├──────────────────────────────>│
 │  KR [B0] OK [01,05,0D]       │  ← enabled=1, 5 rows, 13 cols
 │<──────────────────────────────┤
 │                               │
 │  KR [B0] OK [02,03,01]       │  ← row 2 col 3 pressed
 │<──────────────────────────────┤
 │  KR [B0] OK [02,03,00]       │  ← row 2 col 3 released
 │<──────────────────────────────┤
 │  KR [B0] OK [00,05,01]       │  ← row 0 col 5 pressed
 │<──────────────────────────────┤
 │  ...                          │
 │                               │
 │  KS [B0] (toggle OFF)        │
 ├──────────────────────────────>│
 │  KR [B0] OK [00,05,0D]       │  ← enabled=0, back to normal
 │<──────────────────────────────┤
```

**Usage** : equivalent du [QMK Key Tester](https://config.qmk.fm/#/test). Permet de verifier que chaque touche physique fonctionne et d'identifier les colonnes/rows defectueuses.

#### NVS_RESET (0xB1)
Efface les configurations sauvegardees en NVS et reboot avec les valeurs par defaut.

- Request: `[mask:u8]` — bitmask de ce qu'il faut effacer
- Response: OK puis reboot

**Bitmask :**

| Bit | Valeur | Donnees effacees |
|-----|--------|------------------|
| 0 | 0x01 | Keymaps + noms de layers |
| 1 | 0x02 | Macros |
| 2 | 0x04 | Statistiques (keystats + bigrams) |
| 3 | 0x08 | Tap Dance, Combos, Leader, Key Override |
| 4 | 0x10 | Bluetooth (slots, etat) |
| 5 | 0x20 | Tamagotchi |
| all | 0xFF | Tout effacer |

**Exemples :**
- `KS [B1] [01] [crc]` → efface keymaps uniquement, reboot
- `KS [B1] [09] [crc]` → efface keymaps + features avancees, reboot
- `KS [B1] [FF] [crc]` → factory reset complet, reboot

**Usage** : utile apres un changement de board variant (V2 ↔ V2D) ou quand les keymaps NVS ne correspondent plus au layout physique.

---

### Dongle / Wireless (0xB2–0xB6, dongle role only)

Ces commandes ne sont exposees que sur le firmware dongle (`CONFIG_KASE_DEVICE_ROLE_DONGLE`). Pour detecter le role coter soft, lire la liste de features (KS_CMD_FEATURES) et chercher le tag `RF_DONGLE`. Si absent, le device est un clavier autonome (V1/V2/V2D) — les commandes ci-dessous repondent `ERR_UNKNOWN`.

Tous les champs multi-octets sont little-endian sauf mention contraire.

---

#### RF_PAIR_START (0xB2)
Ouvre une fenetre de pairing de 30 secondes pour accueillir une moitie. Le dongle bascule la radio gauche sur l'adresse/canal de rendezvous et attend un `rf_pair_req`. La reponse part immediatement (non bloquante) ; l'echange continue dans `rf_rx_task`.

- Request: `[reset:u8]`
  - `reset = 0` : ajoute la prochaine moitie aux pairs existants
  - `reset = 1` : efface d'abord toutes les paires NVS, puis ouvre la fenetre
- Response: `[set_id_hi:u8][set_id_lo:u8][paired_count:u8]`
  - `set_id` : identifiant 16-bit du set (derive d'eFuse) — utile pour afficher coter soft
  - `paired_count` : nombre de moities couplees actuellement (0..2)
- Erreur: `ERR_BUSY` si une fenetre est deja ouverte ou si la radio gauche n'est pas presente

Pour savoir si le pairing a abouti, poller `RF_PAIR_LIST` apres ~5–30 s : `paired_count` augmente quand une nouvelle moitie a complete l'echange.

---

#### RF_STATUS (0xB3)
Snapshot complet de l'etat du lien radio pour les deux moities. Idempotent, sans effet de bord — peut etre poll a 1–2 Hz pour piloter un indicateur de barres dans le soft.

- Request: payload vide
- Response: `27 bytes`

| Offset | Type   | Champ           | Description                                                |
|-------:|--------|-----------------|------------------------------------------------------------|
| 0      | u8     | `flags`         | bit0=link_left_up, bit1=link_right_up, bits2-7=reserves    |
| 1      | u8     | `sig_left`      | qualite 0..255 (rf_signal_q255 — 0 = down/timeout)         |
| 2      | u8     | `sig_right`     | idem droite                                                |
| 3..6   | u32 LE | `hb_age_left`   | ms depuis le dernier heartbeat de la moitie gauche         |
| 7..10  | u32 LE | `hb_age_right`  | idem droite                                                |
| 11..14 | u32 LE | `pkt_rx_left`   | nombre total de paquets acceptes (incremente sur succes)   |
| 15..18 | u32 LE | `pkt_rx_right`  | idem droite                                                |
| 19..22 | u32 LE | `pkt_dup_left`  | nombre de duplicats rejetes (seq deja vue)                 |
| 23..26 | u32 LE | `pkt_dup_right` | idem droite                                                |

**Mapping recommande pour 4 barres de signal :**
```
sig >= 200 → 4 barres
sig >= 140 → 3 barres
sig >=  80 → 2 barres
sig >=  30 → 1 barre
sig <   30 → 0 barre / lien casse
```

**Detection de moitie absente** : si `link_<side>_up = 0` ET `pkt_rx_<side> == 0`, la moitie n'a jamais ete vue depuis le boot. Si `link_<side>_up = 0` ET `pkt_rx_<side> > 0`, la moitie a perdu le lien apres avoir fonctionne.

---

#### RF_PAIR_LIST (0xB4)
Liste des MACs des moities actuellement couplees (lecture NVS namespace `rf`).

- Request: payload vide
- Response: `13 bytes`

| Offset | Type    | Champ           | Description                                  |
|-------:|---------|-----------------|----------------------------------------------|
| 0      | u8      | `paired_count`  | 0..2                                         |
| 1..6   | u8[6]   | `mac_left`      | MAC WiFi STA de la moitie gauche (ou zeros)  |
| 7..12  | u8[6]   | `mac_right`     | idem droite                                  |

Si `mac_<side>` vaut `00:00:00:00:00:00`, le slot est libre.

---

#### RF_PAIR_RESET (0xB5)
Efface toutes les paires (NVS namespace `rf` purgee). Les moities couplees expireront leur heartbeat et ne pourront pas se reconnecter sans re-pairing. Utile pour migrer un dongle vers un autre set de moities, ou pour debug.

- Request: payload vide
- Response: `[paired_count:u8]` — 0 apres reset
- Erreur: `ERR_UNKNOWN` si l'ecriture NVS echoue

Le dongle continue de tourner ; la radio garde sa config courante. Pour repeupler les paires, appeler `RF_PAIR_START` avec `reset = 0`.

---

#### BATTERY (0xB6)
Derniere mesure de batterie cachee pour chaque moitie. Les moities n'emettent `EN_INFO_BATTERY` que lorsque la valeur change (rate-limited), donc l'`age_ms` peut grandir legitimement entre deux samples.

- Request: payload vide
- Response: `14 bytes` — 2 enregistrements de 7 octets chacun

Format d'un enregistrement (slot 0 = LEFT, slot 1 = RIGHT) :

| Offset | Type   | Champ      | Description                                      |
|-------:|--------|------------|--------------------------------------------------|
| 0      | u8     | `batt_dV`  | Tension × 10 (volts × 10). `0xFF` = jamais vu    |
| 1      | u8     | `soc_pct`  | State of charge 0..100. `0xFF` = inconnu         |
| 2      | u8     | `charging` | 0 = decharge, 1 = en charge. `0xFF` = inconnu    |
| 3..6   | u32 LE | `age_ms`   | ms depuis la derniere mise a jour. `0xFFFFFFFF` = jamais recu |

**Regles d'affichage coter soft :**
- Si `batt_dV == 0xFF` OU `soc_pct == 0xFF` → afficher "—" / placeholder
- Si `age_ms > 60000` (1 minute) → griser la valeur (potentiellement obsolete)
- Si `age_ms == 0xFFFFFFFF` → la moitie ne supporte pas encore la telemetrie batterie (firmware ancien ou pile de batterie pas encore branchee)

---

#### MONITOR (0xB7)
Snapshot consolide de l'etat live du clavier (et de ses moities sans fil). Concu pour piloter un tableau de bord de monitoring dans KaSe_soft. Idempotent, sans effet de bord — poll recommande a **1–2 Hz**.

- Request: payload vide (`KS [B7] 0000 00`)
- Response: `28 bytes` — toujours OK, sentinelles pour les champs sans source disponible

| Offset | Type   | Champ          | Description                                                       |
|-------:|--------|----------------|-------------------------------------------------------------------|
| 0      | u8     | `fmt`          | = `0x01` — version du format, permet evolution future             |
| 1      | u8     | `flags`        | bitmask (voir ci-dessous)                                         |
| 2      | u32 LE | `uptime_s`     | Secondes depuis le boot                                           |
| 6      | u16 LE | `heap_free_kb` | Heap libre en KB (sature a 0xFFFF)                                |
| 8      | i8     | `temp_c`       | Temperature interne degC ; `INT8_MIN` (−128) = pas de capteur     |
| 9      | u8     | `layer_idx`    | Index du layer actif                                              |
| 10     | u8     | `wpm`          | Mots par minute courants (sature a 255)                           |
| 11     | u32 LE | `keys_total`   | Nombre total de touches pressees, cumul a vie (restaure depuis NVS `key_stats_tot` au boot) |
| 15     | u8     | `sig_left`     | Qualite du lien RF gauche 0..255 (0 si `has_rf=0`)                |
| 16     | u8     | `sig_right`    | Idem droite                                                       |
| 17     | u16 LE | `hb_age_L_ms`  | ms depuis le dernier heartbeat de la moitie gauche (tronque a u16, sature a 0xFFFF — RF_STATUS utilise u32 pour ces champs) |
| 19     | u16 LE | `hb_age_R_ms`  | Idem droite (meme troncature u16/0xFFFF)                          |
| 21     | u8     | `batt_L_dV`    | Tension batterie gauche x10 (0 = inconnu)                         |
| 22     | u8     | `batt_L_soc`   | State of charge gauche 0..100 %                                   |
| 23     | u8     | `batt_L_chg`   | 0 = decharge, 1 = en charge                                       |
| 24     | u8     | `batt_R_dV`    | Tension batterie droite x10 (0 = inconnu)                         |
| 25     | u8     | `batt_R_soc`   | State of charge droite 0..100 %                                   |
| 26     | u8     | `batt_R_chg`   | 0 = decharge, 1 = en charge                                       |
| 27     | u8     | `bt_slot`      | Slot Bluetooth actif (0–2 ; `BT_MAX_DEVICES = 3`)                 |

Note: dans ce snapshot, les champs batterie utilisent 0 = inconnu (et non 0xFF comme la commande BATTERY 0xB6) — format consolide simplifie.

**Flags (offset 1) :**

| Bit | Masque | Constante    | Signification                       |
|-----|--------|--------------|-------------------------------------|
| 0   | 0x01   | `HAS_RF`     | Firmware role dongle avec radio RF  |
| 1   | 0x02   | `LINK_L`     | Moitie gauche connectee             |
| 2   | 0x04   | `LINK_R`     | Moitie droite connectee             |
| 3   | 0x08   | `USB`        | Lien USB actif                      |
| 4   | 0x10   | `BT_CONN`    | Bluetooth connecte                  |

**Clavier autonome (sans dongle) :** `has_rf = 0`, les offsets 15..26 (signal RF, heartbeat, batterie) valent zero. Le soft detecte la presence RF via le flag `HAS_RF` (et/ou le tag `RF_DONGLE` dans la reponse FEATURES).

**Temperature :** `temp_c = INT8_MIN` (−128) signifie que le capteur n'est pas disponible.

**Exemple de parsing Python :**

```python
import struct

def parse_monitor(payload: bytes) -> dict:
    assert len(payload) == 28
    fmt, flags = payload[0], payload[1]
    uptime,    = struct.unpack_from("<I", payload, 2)
    heap_kb,   = struct.unpack_from("<H", payload, 6)
    temp       = struct.unpack_from("<b", payload, 8)[0]
    layer, wpm = payload[9], payload[10]
    keys,      = struct.unpack_from("<I", payload, 11)
    sig_l, sig_r = payload[15], payload[16]
    hb_l,      = struct.unpack_from("<H", payload, 17)
    hb_r,      = struct.unpack_from("<H", payload, 19)
    bl_dv, bl_soc, bl_chg = payload[21], payload[22], payload[23]
    br_dv, br_soc, br_chg = payload[24], payload[25], payload[26]
    bt_slot    = payload[27]
    has_rf     = bool(flags & 0x01)
    return dict(
        fmt=fmt, flags=flags, uptime_s=uptime, heap_free_kb=heap_kb,
        temp_c=temp if temp != -128 else None,
        layer_idx=layer, wpm=wpm, keys_total=keys,
        sig_left=sig_l if has_rf else None,
        sig_right=sig_r if has_rf else None,
        hb_age_L_ms=hb_l, hb_age_R_ms=hb_r,
        batt_L=(bl_dv / 10, bl_soc, bool(bl_chg)) if bl_dv else None,
        batt_R=(br_dv / 10, br_soc, bool(br_chg)) if br_dv else None,
        bt_slot=bt_slot,
    )
```

**Exemple de parsing C# (KaSe_soft) :**

```csharp
public class MonitorSnapshot
{
    // Raw fields
    public byte   Fmt        { get; init; }
    public byte   Flags      { get; init; }
    public uint   UptimeS    { get; init; }
    public ushort HeapFreeKb { get; init; }
    public sbyte  TempC      { get; init; }
    public byte   LayerIdx   { get; init; }
    public byte   Wpm        { get; init; }
    public uint   KeysTotal  { get; init; }
    public byte   SigLeft    { get; init; }
    public byte   SigRight   { get; init; }
    public ushort HbAgeLeftMs  { get; init; }
    public ushort HbAgeRightMs { get; init; }
    public byte   BattLdV    { get; init; }
    public byte   BattLSoc   { get; init; }
    public byte   BattLChg   { get; init; }
    public byte   BattRdV    { get; init; }
    public byte   BattRSoc   { get; init; }
    public byte   BattRChg   { get; init; }
    public byte   BtSlot     { get; init; }

    // Flag helpers
    public bool HasRf      => (Flags & 0x01) != 0;
    public bool LinkLeft   => (Flags & 0x02) != 0;
    public bool LinkRight  => (Flags & 0x04) != 0;
    public bool UsbActive  => (Flags & 0x08) != 0;
    public bool BtConnected => (Flags & 0x10) != 0;

    // Derived
    public bool TempAvailable => TempC != -128;
    public double BattLVoltage => BattLdV / 10.0;
    public double BattRVoltage => BattRdV / 10.0;

    public static MonitorSnapshot Parse(byte[] p)
    {
        // All multi-byte fields are little-endian.
        // BitConverter is LE on all modern platforms; assert if needed:
        // if (!BitConverter.IsLittleEndian) throw new PlatformNotSupportedException();
        if (p.Length < 28) throw new ArgumentException("payload must be 28 bytes");
        return new MonitorSnapshot
        {
            Fmt          = p[0],
            Flags        = p[1],
            UptimeS      = BitConverter.ToUInt32(p, 2),
            HeapFreeKb   = BitConverter.ToUInt16(p, 6),
            TempC        = (sbyte)p[8],
            LayerIdx     = p[9],
            Wpm          = p[10],
            KeysTotal    = BitConverter.ToUInt32(p, 11),
            SigLeft      = p[15],
            SigRight     = p[16],
            HbAgeLeftMs  = BitConverter.ToUInt16(p, 17),
            HbAgeRightMs = BitConverter.ToUInt16(p, 19),
            BattLdV      = p[21],
            BattLSoc     = p[22],
            BattLChg     = p[23],
            BattRdV      = p[24],
            BattRSoc     = p[25],
            BattRChg     = p[26],
            BtSlot       = p[27],
        };
    }
}
```

**Usage dashboard recommande :** timer 1–2 Hz → `KS [B7] 0000 00` → `Parse(response.Payload)` → mettre a jour les bindings WPF. Pas de gestion d'erreur necessaire (la commande retourne toujours OK).

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
