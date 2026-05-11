# KaSe Dongle Firmware — Design

**Date** : 2026-05-11
**Branche cible** : `dongle-firmware` (à créer dans `KaSe_firmware`)
**Status** : Design — en attente de review utilisateur
**Hardware** : `~/Documents/PCB-esp/dongle/dongle/` (PCB figé, schéma KiCad)
**North star** : faire évoluer `KaSe_firmware` vers un framework type QMK/ZMK ; le dongle est le 1er pas. Refactor minimal en v1.

---

## 1. Vue d'ensemble & cadrage

Ajouter un firmware pour le dongle USB du KaSe split wireless dans le repo `KaSe_firmware`, sur une branche dédiée. Le dongle reçoit les events de 2 halves via NRF24L01+, exécute le **keymap engine complet** (porté du firmware existant), et présente un USB composite (HID NKRO + mouse + consumer + CDC binaire) à l'hôte via un hub interne CH334R.

Stratégie repo :
- Branche `dongle-firmware` dans `~/Documents/GitHub/KaSe_firmware`
- Nouveau board variant `boards/kase_dongle/`
- Build : `idf.py -B build_dongle -DBOARD=kase_dongle build`
- Les 3 boards existants (V1/V2/V2D) **ne sont pas touchés** en surface — leurs binaires post-merge doivent être identiques aux pré-merge

Refactor minimal pour cette branche :
1. **`device_role`** (Kconfig) : `KEYBOARD` ou `DONGLE`. Conditionne les modules compilés.
2. **`input_source_t`** : enum `{LOCAL_MATRIX, RF_HALF_LEFT, RF_HALF_RIGHT}` passée à l'engine avec chaque event. Permet de mapper logiquement (row,col) global selon la moitié.

Pas de refactor en components ESP-IDF. Pas de transport HAL. Pas de behavior system. On factorisera quand un 3ème device émergera.

**Hors scope v1** : Tamagotchi sur dongle, OTA halves via NRF (utilise ESP-NOW à la place), push dongle→halves pour layer state e-ink, BLE, display, framework "à la ZMK". Voir Section 13.

---

## 2. Hardware refresher

Dongle ESP32-S3-WROOM-2 (module soudé), alim 3.3V depuis le slot M.2 (Key B 3042, format 22×42 mm).

**SPI bus partagé entre les 2 NRF24L01+** :
- MOSI / MISO / SCK : à confirmer dans le schéma `dongle/dongle/dongle.kicad_sch` (3 GPIO ESP32-S3 communs)
- NRF#1 (half gauche) : signaux `CSN_1`, `CE_1`, `IRQ_1`
- NRF#2 (half droit) : signaux `CSN_2`, `CE_2`, `IRQ_2`
- IRQ pins câblées séparément → 2 ISR distincts, RX en parallèle (sérialisé sur SPI mais négligeable)

**USB ESP32-S3 OTG full-speed** → CH334R hub (port 1) → host. CH340C sur port 2 du hub pour flash/debug UART (auto-reset via UMH3N sur DTR/RTS → EN/IO0). En production, l'hôte ne voit qu'un device USB composite ; le CH340C est invisible côté HID.

**Ce qui n'existe PAS sur le dongle** : pas de matrix locale, pas de display, pas de LED, pas d'usage BLE (radio Bluetooth désactivée), pas de batterie.

**À faire en début d'implémentation** : extraire le mapping exact des signaux NRF/SPI vers les GPIO ESP32-S3 depuis le schéma KiCad, le coder dans `boards/kase_dongle/board.h`.

---

## 3. Architecture FreeRTOS & flux de données

```
                    ┌──────────────────────────────────────┐
                    │            ESP32-S3 dongle           │
                    │                                      │
USB host            │  ┌──────────┐    ┌──────────────┐   │   2.4GHz
   ◄──── HID ──────►│  │ tinyusb  │◄──►│ rf_rx_task   │◄──┼──────────┐
   ◄──── CDC ──────►│  │   task   │    │ (engine in)  │   │  NRF24   │
                    │  └────┬─────┘    └──────┬───────┘   │  ESB     │
                    │       │                 │           │   ┌────────┴──┐
                    │       │                 │ HID q     │   │NRF24L01_L │◄═══ Half L (ch 76)
                    │       ▼                 │           │   │(IRQ_1)    │
                    │    cdc_task             │           │   ├───────────┤
                    │       │                 │           │   │NRF24L01_R │◄═══ Half R (ch 82)
                    │       ▼                 │           │   │(IRQ_2)    │
                    │  ┌──────────┐    ┌──────▼───────┐   │   └────────┬──┘
                    │  │espnow_task│   │ stats_task   │   │            │
                    │  │(cold path)│   │  (NVS write) │   │   WiFi     │
                    │  └──────────┘    └──────────────┘   │ internal   │
                    └──────────────────────────────────────┘
```

### Tasks FreeRTOS

| Task | Prio | Stack | Core | Rôle |
|---|---|---|---|---|
| `rf_rx_task` | 10 | 8KB | 0 | Wait sur sem signalé par les 2 IRQ NRF, lit FIFO RX, parse, **process inline dans la task** (engine + HID build), push vers `hid_q`. Pas de hop intermédiaire pour minimiser la latence. |
| `tinyusb` (interne) | 8 | 4KB | 0 | Driver TinyUSB, consomme `hid_q`, envoie HID; dispatch CDC vers `cdc_q`. |
| `cdc_task` | 5 | 6KB | 1 | Parse trames KS binaires, dispatch handlers, génère KR. Peut déclencher `espnow_task` ou config push. |
| `espnow_task` | 4 | 8KB | 1 | Cold path : OTA chunks vers halves, config push, telemetry RX. Activé seulement on demand. |
| `stats_task` | 3 | 2KB | 1 | Persiste key_stats + bigrams en NVS, throttled (~30s). |

### ISRs

`nrf_irq_isr_left/right` : `IRAM_ATTR`, déclenche `xSemaphoreGiveFromISR(nrf_evt_sem)` (sem commun aux deux). Aucune SPI dans l'ISR.

### Flux hot path (target ≤1ms typique, ≤3ms p99)

```
half TX → NRF on-air ~100µs → NRF_RX FIFO → IRQ → rf_rx_task wakes (~10µs)
  → SPI read FIFO (~30µs @ 10MHz) → parse → engine processing inline (~200µs)
  → HID report build (~50µs) → hid_q push → tinyusb_task → USB IN endpoint (~50µs)
  → host poll (0-1ms, typique 500µs)
```

Budget total : ~440µs dongle-side + 0-1000µs USB poll = **~1ms typique**, ~2.5ms pire (poll manqué + 1 retry NRF).

### Hypothèses de performance

- NRF SPI à 10 MHz (max datasheet)
- Payload event minimal : 3 bytes (vs 32B max NRF)
- Pas de `malloc` dans `rf_rx_task`
- Pas de `ESP_LOGI` dans le hot path (uniquement `ESP_LOGD` compilé out en prod)
- Engine lookups O(1) array (déjà le cas dans KaSe_firmware)
- CPU 240 MHz fixe, pas de PM/DFS

### Synchro état

`rf_rx_task` maintient un bitmap `currently_pressed[half_id]` mis à jour à chaque `PKT_KEY` reçu. Le heartbeat (toutes les 100ms par half) compare au bitmap reçu → release ou press forcé pour reconciler. Voir Section 4 pour le détail.

### Transports

- **NRF24** : always-on RX, hot path, événements clavier + heartbeat. Latence prioritaire.
- **ESP-NOW** : on-demand, démarré par commande CDC (OTA push, config push). Mute le NRF pendant les fenêtres ESP-NOW pour limiter coexistence radio.

---

## 4. Protocole RF (NRF24L01+)

**Mode** : Enhanced ShockBurst (ESB), 2 Mbps, dynamic payload length (DPL), auto-ACK, ARC=3 retries, ARD=250µs.

**Canaux** : **2 canaux distincts**, un par half — élimine la contention air-time entre halves et donne résilience séparée aux interférences.

| Half | Canal default | Fréquence | NRF physique sur dongle |
|---|---|---|---|
| Gauche | `0x4C` (76) | 2476 MHz | NRF#1 |
| Droit | `0x52` (82) | 2482 MHz | NRF#2 |

Espacement 6 MHz (>2 MHz requis en mode 2 Mbps). Au-dessus du WiFi ch 11 (2462 MHz centre).

**Adressing** :
- Largeur d'adresse : 5 bytes
- Adresses distinctes par half (défense en profondeur même avec canaux séparés) :
  ```
  rf_addr_left  = [BASE 4B][0x01]
  rf_addr_right = [BASE 4B][0x02]
  ```
- `BASE` : 4 bytes en NVS `rf.addr_base`. Default factory = `KASE` ascii.

**Packet types** (1er nibble du byte 0 = type, 2ème nibble = flags) :

| Type | ID | Direction | Taille | Payload |
|---|---|---|---|---|
| `PKT_KEY` | 0x1 | half→dongle | 3B | `[type\|flags][row<<4\|col][seq8]` |
| `PKT_HEARTBEAT` | 0x2 | half→dongle | 9B | `[type\|flags][bitmap×5B][batt_dV][link_q][seq8]` |
| `PKT_TRACKPAD` | 0x3 | half_R→dongle | 7B | `[type\|flags][dx_i8][dy_i8][btns3\|scroll_v_i8][scroll_h_i8][seq8]` |
| `PKT_PAIR_REQ` | 0xF | half→dongle (pairing channel) | 8B | Réservé pour runtime pairing futur |

**Champs flags** :
- `flags.b0` = `pressed` (pour `PKT_KEY`)
- `flags.b1` = `is_retry` (positionné par half lors de retransmit applicatif si ESB échoue après ARC=3)
- `flags.b2-b3` = réservé

**Anti-stuck logic (`rf_rx_task`)** :

```text
on PKT_KEY received from half H:
    if seq == last_seq[H] → drop (dup)
    else: process_event(H, row, col, pressed); last_seq[H] = seq

on PKT_HEARTBEAT received from H:
    expected = current_pressed_local[H]
    actual   = heartbeat.bitmap
    for each pos in (expected XOR actual):
        if expected.bit(pos) && !actual.bit(pos): force_release(H, pos)
        if !expected.bit(pos) && actual.bit(pos): force_press(H, pos)  // missed event
    last_hb_ts[H] = now

on heartbeat timeout (>250ms no hb from H):
    release all keys held by H
    set link_state[H] = LOST → remonté via CDC pour notif controller
```

**Heartbeat content (9B)** :
- Période 100ms par half
- Bitmap : 5 rows × 7 cols = 35 bits packés MSB-first (row 0 col 0 = bit 0 du byte 0), 5 bytes
- `batt_dV` : 0-83 → 0-8.3V (résolution 0.1V, 0 = unknown)
- `link_q` : nb retries cumulés depuis dernier heartbeat (0 = parfait, 255 = link mort)

**Coexistence ESP-NOW** : quand le dongle active ESP-NOW (cold path), on suspend les NRF (`ce_low()` + skip RX FIFO check) pour la fenêtre. Halves continuent à TX, leurs packets sont perdus → reconciliation au prochain heartbeat. Acceptable pour OTA/config qui sont user-initiated.

**Pas de frequency hopping en v1**. Si interférences observées en bench, on l'ajoutera Phase 2.

---

## 5. USB stack (TinyUSB composite)

Topologie : 2 interfaces logiques (3 descripteurs d'interface) :

| Interface | Class | Sub | Endpoints | Notes |
|---|---|---|---|---|
| 0 | HID (vendor) | none | 1× INT IN | NKRO + mouse + consumer + system via Report IDs |
| 1 | CDC control | ACM | 1× INT IN | Notification |
| 2 | CDC data | none | 1× BULK IN, 1× BULK OUT | Data CDC binaire |

Bilan endpoints : 3 IN + 1 OUT + EP0. ESP32-S3 OTG full-speed supporte 5 IN + 5 OUT — confortable.

**Single HID interface, multiple Report IDs** :

| Report ID | Type | Description |
|---|---|---|
| 1 | NKRO Keyboard IN | Modifier byte + bitmap 14B (112 keys, USB HID Usage Page Keyboard 0x07 codes 0-111) |
| 2 | Mouse IN | Buttons (3 bits) + dx/dy (int16) + wheel_v (int8) + wheel_h (int8) |
| 3 | Consumer IN | 16-bit usage code (volume, play/pause, brightness…) |
| 4 | System IN | 8-bit usage code (sleep, wake, power) |
| 5 | Keyboard OUT | LEDs (Caps/Num/Scroll/Compose/Kana) — facultatif, route vers ESP-NOW push half si on veut afficher caps lock plus tard (Phase 2) |

**Pas de boot protocol** : le dongle vit en M.2 d'un laptop, pas de BIOS context où le boot keyboard servirait. Skip = code plus simple, descripteur plus court.

**bInterval=1** sur l'INT IN HID (1ms polling host) — clé pour la latence cible.

**CDC** : binaire identique à KaSe_firmware (frames KS/KR + CRC-8). Reuse `cdc_acm_com.c`, `cdc_binary_protocol.c`, `cdc_binary_cmds.c` quasi tels quels. Nouveaux handlers ajoutés dans `cdc_dongle_cmds.c` (Section 9).

**Identification USB** :
- VID/PID : à finaliser. Suggestion dev : `0x1209` (pid.codes) avec un PID alloué, ou `0x303A` (Espressif) en attendant.
- Strings : Manufacturer="KaSe", Product="KaSe Dongle", Serial = MAC ESP32 stringifiée.
- Power : 100 mA bus-powered, pas de remote wakeup.

**Auto-reset CH340C** : géré côté hub par UMH3N → invisible firmware.

---

## 6. NVS layout

Namespace `"storage"` (existant) pour partager les utilities `nvs_save_blob_with_total()`. Nouveau namespace `"rf"` pour la config radio (isolé pour reset partiel possible).

### Namespace `rf` (nouveau)

| Clé | Type | Taille | Default | Notes |
|---|---|---|---|---|
| `addr_base` | blob | 4B | `KASE` ascii | Préfixe adresse NRF, +0x01/0x02 pour L/R |
| `ch_left` | u8 | 1B | 0x4C | Canal NRF half gauche |
| `ch_right` | u8 | 1B | 0x52 | Canal NRF half droit |
| `mac_left` | blob | 6B | zero | MAC ESP-NOW half gauche |
| `mac_right` | blob | 6B | zero | MAC ESP-NOW half droit |
| `wifi_ch` | u8 | 1B | 11 | Canal WiFi pour ESP-NOW (cold path), distinct des NRF |
| `paired` | u8 | 1B | 0 | 0=factory defaults, 1=configured via CDC |

### Namespace `storage` (réutilise + ajoute)

| Clé | Origine | Notes dongle |
|---|---|---|
| `keymaps`, `layout_names` | KaSe_firmware | Indexation par position globale (0..69) |
| `macros`, `td_configs`, `combo_cfg`, `leader_cfg`, `ko_cfg` | KaSe_firmware | Inchangé |
| `key_stats`, `key_stats_tot`, `bigram_stats`, `bigram_total` | KaSe_firmware | Compteurs sur 70 positions |
| `bt_*` | KaSe_firmware | **Non utilisé** (BLE off sur dongle) — restera vide |
| `tama_*` | KaSe_firmware | **Non utilisé** sur dongle |

### Partition table

**Inchangée** — on garde `partitions.csv` existant (16 MB layout). NVS 64 KB suffit (~21 KB bigrams + keymaps + macros + ~30 B RF config). Le board variant `kase_dongle` réutilise la même partition table.

### Mapping matriciel global (pour l'engine)

```
Global pos (0..69) :
  half_left  : row 0..4, col 0..6   → indices 0..34
  half_right : row 0..4, col 0..6   → indices 35..69
```

Layout JSON dans `boards/kase_dongle/board_layout.c` (style `boards/kase_v2/board_layout.c`).

---

## 7. Intégration du keymap engine

L'engine existant (`main/input/key_processor.c`, `key_features.c`, `tap_hold.c`, `tap_dance.c`, `combo.c`, `leader.c`, `hid_report.c`, `key_stats.c`) reçoit aujourd'hui des events depuis `matrix_scan.c`. On ajoute une **source d'entrée alternative** sans toucher à la logique métier.

### API minimum modifiée

Aujourd'hui : `process_key_event(uint8_t row, uint8_t col, bool pressed)` — implicit "row/col local matrix".

Nouveau :

```c
typedef enum {
    INPUT_SRC_LOCAL_MATRIX = 0,  // V1/V2/V2D : matrice locale
    INPUT_SRC_RF_LEFT      = 1,  // dongle : half gauche via NRF
    INPUT_SRC_RF_RIGHT     = 2,  // dongle : half droit via NRF
} input_source_t;

void process_key_event(input_source_t src, uint8_t row, uint8_t col, bool pressed);
```

À l'intérieur :

```c
static inline uint8_t to_global_pos(input_source_t src, uint8_t row, uint8_t col) {
    uint8_t local = row * COLS_PER_HALF + col;
    switch (src) {
        case INPUT_SRC_LOCAL_MATRIX: return local;       // V1/V2 : ≤35 keys
        case INPUT_SRC_RF_LEFT:      return local;        // dongle : 0..34
        case INPUT_SRC_RF_RIGHT:     return 35 + local;   // dongle : 35..69
    }
}
```

À partir de là toute la logique en aval reste intacte.

### Cross-half features (par construction)

- **Layer stack** : single, sur le dongle. `MO(layer)` sur half_L active la layer pour toutes positions.
- **Combos cross-half** : un combo référence des positions globales — fonctionne dès que les positions sont uniques.
- **Modifiers latching** : modifier maintenu sur half_L et touche déclenchée sur half_R → fonctionne (HID report agrège tout).
- **Tap-hold** : timing géré par tick FreeRTOS global, indépendant de la source.

### Code modifié dans l'engine

1. `key_processor.c` : ajouter param `input_source_t` à `process_key_event`, mapper en pos globale en entrée, push down inchangé.
2. `key_definitions.h` / `keymap.c` : étendre `MAX_MATRIX_KEYS` à 70 pour le board dongle (déjà conditionné par `BOARD_*` en V1/V2/V2D, on ajoute le cas dongle).
3. `matrix_scan.c` : **non compilé** sur dongle (Kconfig `CONFIG_KASE_HAS_LOCAL_MATRIX=n`).
4. `keyboard_task.c` : **non compilé** sur dongle (équivalent = `rf_rx_task`).

### Code créé pour le dongle

Sous `main/comm/rf/` (nouveau) :
- `rf_driver.c/h` : driver bas-niveau NRF24L01+ (init, RX FIFO, IRQ, switch channel, set address). Une instance par NRF physique.
- `rf_rx_task.c` : la task qui multiplexe les 2 NRFs, parse les packets, appelle `process_key_event` ou `trackpad_handle` ou `heartbeat_reconcile`.
- `rf_packet.h` : structs des packet types + helpers d'encoding/parsing.
- `heartbeat.c` : state-reconciliation logic (timeouts, force-release, link state).

### HID report sender

`hid_report.c` reste inchangé (consume `hid_q`, push vers TinyUSB). Sur le dongle il route TOUT vers USB (BLE désactivé). `hid_transport.c` se réduit à `hid_send_to_usb()` direct ; `usb_bl_state` devient no-op ou compile-out.

---

## 8. ESP-NOW cold path (OTA + config + telemetry verbose)

Modèle d'activation : ESP-NOW est **off par défaut**. Activé on-demand par commande CDC, run pour la durée de l'opération, puis stoppé. Évite la coexistence permanente NRF/WiFi.

### Init différé

```c
esp_now_init() + esp_now_register_recv_cb()  // appelé à la 1ère commande CDC qui en a besoin
esp_now_add_peer(mac_left)
esp_now_add_peer(mac_right)
// peers chargés depuis NVS rf.mac_left / rf.mac_right
```

WiFi en mode `WIFI_MODE_STA` mais sans connexion (juste pour activer le radio). Channel fixé sur `wifi_ch` NVS, distinct des canaux NRF.

### Pendant ESP-NOW actif

1. `rf_rx_task` reste vivant mais on baisse la puissance TX des NRF + on logge les misses (presence beacon manqué via heartbeat → release auto déjà géré).
2. Engine continue à fonctionner. Si l'user tape pendant un OTA, les events RF qui passent sont processés ; les autres sont récupérés au prochain heartbeat (force-press si manqué).
3. Acceptable car cold path = opérations user-initiated rares.

### Packet protocol ESP-NOW (max 250B payload)

| Type | ID | Direction | Payload |
|---|---|---|---|
| `EN_OTA_BEGIN` | 0x10 | dongle→half | `[total_size:u32][crc32:u32][version:6B]` |
| `EN_OTA_CHUNK` | 0x11 | dongle→half | `[chunk_idx:u16][data:up to 240B]` |
| `EN_OTA_END` | 0x12 | dongle→half | `[final_crc:u32]` |
| `EN_OTA_ACK` | 0x13 | half→dongle | `[chunk_idx:u16][status:u8]` (0=ok, 1=crc_fail, 2=write_fail) |
| `EN_CFG_PUSH` | 0x20 | dongle→half | `[cfg_id:u8][len:u16][data]` |
| `EN_CFG_ACK` | 0x21 | half→dongle | `[cfg_id:u8][status:u8]` |
| `EN_TELEM_REQ` | 0x30 | dongle→half | empty |
| `EN_TELEM_RSP` | 0x31 | half→dongle | `[batt_dV][rssi][temp_c][uptime_s:u32][rf_retries:u32][...]` |

### Throughput estimé

ESP-NOW peer unicast avec ACK ~1 Mbps. Avec chunks 240B + overhead protocol (~10B header + ESP-NOW frame ~30B), efficient ~80%. Pour 1.5 MB binaire half : **~15 s**.

### OTA half flow (côté dongle)

1. CDC reçoit `KS_CMD_HALF_OTA_BEGIN` (target_half, total_size, crc, version).
2. Dongle init ESP-NOW si pas déjà fait, suspend NRF TX du target.
3. Boucle CDC RX → ESP-NOW TX :
   - CDC `KS_CMD_HALF_OTA_CHUNK` (idx, data) → dongle forward `EN_OTA_CHUNK` → wait `EN_OTA_ACK` (timeout 100 ms, retry 3×) → KR ack au controller
4. CDC `KS_CMD_HALF_OTA_END` → forward `EN_OTA_END`, attendre confirmation, KR final.
5. Dongle re-active NRF, désactive ESP-NOW.

### Failure modes

- Half ne répond pas à un chunk → 3 retries → abort, KR error → controller re-tente plus tard
- Half répond CRC fail sur un chunk → re-send ce chunk
- Power loss côté half pendant OTA → half a sa propre logique safe-boot qui retombe sur partition factory

### Config push

Même logique, payload plus court, idempotent.

### Telemetry verbose

Commande `KS_CMD_HALF_TELEMETRY_GET` (target_half) → dongle envoie `EN_TELEM_REQ` → reçoit `EN_TELEM_RSP` → KR au controller. Single-shot, pas de stream.

---

## 9. Additions au protocole CDC binaire

Le protocole KS/KR CRC-8 reste identique (`docs/CDC_BINARY_PROTOCOL.md`). On **réserve un bloc d'IDs `0xC0-0xCF`** pour les commandes spécifiques dongle.

| ID | Nom | Direction | Payload KS | Payload KR | Notes |
|---|---|---|---|---|---|
| `0xC0` | `KS_CMD_RF_GET_CONFIG` | host→dongle | empty | `[addr_base:4B][ch_L:u8][ch_R:u8][mac_L:6B][mac_R:6B][wifi_ch:u8][paired:u8]` | Lit la config RF actuelle |
| `0xC1` | `KS_CMD_RF_SET_CONFIG` | host→dongle | `[addr_base:4B][ch_L:u8][ch_R:u8][mac_L:6B][mac_R:6B][wifi_ch:u8]` | empty | Persiste en NVS, set `paired=1`, restart RF stack |
| `0xC2` | `KS_CMD_RF_RESET_FACTORY` | host→dongle | empty | empty | Reset NVS namespace `rf` aux defaults |
| `0xC3` | `KS_CMD_RF_LINK_STATUS` | host→dongle | empty | `[link_L:u8][link_R:u8][hb_age_L:u16][hb_age_R:u16][rssi_proxy_L][rssi_proxy_R][last_seq_L][last_seq_R]` | Diagnostic temps réel |
| `0xC4` | `KS_CMD_HALF_OTA_BEGIN` | host→dongle | `[target:u8][total_size:u32][crc32:u32][version:6B]` | empty | target: 0=L, 1=R |
| `0xC5` | `KS_CMD_HALF_OTA_CHUNK` | host→dongle | `[chunk_idx:u16][data:up to 240B]` | `[chunk_idx:u16]` | Streamed |
| `0xC6` | `KS_CMD_HALF_OTA_END` | host→dongle | `[target:u8]` | `[final_status:u8]` | Sortie mode OTA, ré-active NRF |
| `0xC7` | `KS_CMD_HALF_OTA_ABORT` | host→dongle | `[target:u8]` | empty | Bail-out user |
| `0xC8` | `KS_CMD_HALF_CONFIG_PUSH` | host→dongle | `[target:u8][cfg_id:u8][len:u16][data]` | `[cfg_status:u8]` | Trackpad params, brightness e-ink, etc. |
| `0xC9` | `KS_CMD_HALF_TELEMETRY_GET` | host→dongle | `[target:u8]` | `[batt_dV][rssi][temp_c][uptime_s:u32][rf_retries:u32]` | Single-shot |
| `0xCA` | `KS_CMD_DONGLE_STATS` | host→dongle | empty | `[uptime_s:u32][rf_pkt_rx_L:u32][rf_pkt_rx_R:u32][rf_dup_L:u16][rf_dup_R:u16][hid_sent:u32][cdc_rx_bytes:u32]` | Diagnostic dongle |

**Status codes** : réutilise les existants + nouveau `0x06=ERR_LINK_DOWN`.

**Mode mutex** : `KS_CMD_HALF_OTA_BEGIN` met le dongle en `mode = OTA_<half>`. Tant qu'on est dans ce mode :
- Les commandes RF/CONFIG vers ce half sont rejetées (`ERR_BUSY`)
- Les events RF du half ciblé sont ignorés (le half est en train d'être flashé)
- L'autre half continue à fonctionner normalement (canal NRF distinct)

**Backward compat** : aucune commande `0x00-0xBF` n'est modifiée. Tout le KaSe_controller existant continue de marcher contre un dongle (modulo les commandes liées à BLE/Tama qui retournent `ERR_INVAL` ou sont strippées).

**Doc** : nouveau fichier `docs/CDC_BINARY_PROTOCOL_DONGLE.md` qui liste les ajouts.

---

## 10. Build system & board variant `kase_dongle`

### Structure ajoutée

```
boards/
├── kase_v1/
├── kase_v2/
├── kase_v2_debug/
└── kase_dongle/              ← nouveau
    ├── board.h               // pinout NRF + define KASE_DEVICE_ROLE_DONGLE
    ├── board_keymap.c        // keymap par défaut sur 70 positions
    ├── board_layout.c        // layout JSON pour les 2 halves
    ├── board_features.h      // flags : HAS_LOCAL_MATRIX=0, HAS_DISPLAY=0, HAS_BLE=0, HAS_RF_RX=1, HAS_ESPNOW=1
    └── README.md             // pinout dongle + notes
```

### Build commands

```bash
idf.py -B build_dongle -DBOARD=kase_dongle build
idf.py -B build_dongle -p /dev/ttyUSB0 flash  # via CH340C sur port 2 du hub
```

### CMake conditionnel

`main/CMakeLists.txt` rendu conditionnel via Kconfig flags :

```cmake
set(srcs main.c)

if(CONFIG_KASE_HAS_LOCAL_MATRIX)
    list(APPEND srcs input/matrix_scan.c input/keyboard_task.c)
endif()
if(CONFIG_KASE_HAS_DISPLAY)
    list(APPEND srcs display/status_display.c ...)
endif()
if(CONFIG_KASE_HAS_BLE)
    list(APPEND srcs comm/ble/hid_bluetooth_manager.c)
endif()
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs comm/rf/rf_driver.c comm/rf/rf_rx_task.c comm/rf/heartbeat.c)
endif()
if(CONFIG_KASE_HAS_ESPNOW)
    list(APPEND srcs comm/espnow/espnow_task.c comm/espnow/espnow_ota.c ...)
endif()

# Engine et HID toujours compilés
list(APPEND srcs input/key_processor.c input/key_features.c input/tap_hold.c
                 input/tap_dance.c input/combo.c input/leader.c
                 input/hid_report.c input/keymap.c input/key_stats.c)

# CDC toujours compilé
list(APPEND srcs comm/cdc/cdc_acm_com.c comm/cdc/cdc_binary_protocol.c
                 comm/cdc/cdc_binary_cmds.c comm/cdc/cdc_ota.c)
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs comm/cdc/cdc_dongle_cmds.c)  # handlers 0xC0-0xCF
endif()
```

### Kconfig (`main/Kconfig.projbuild`)

```
choice KASE_DEVICE_ROLE
    prompt "KaSe device role"
    default KASE_DEVICE_ROLE_KEYBOARD
    config KASE_DEVICE_ROLE_KEYBOARD
        bool "Keyboard (V1/V2/V2D)"
    config KASE_DEVICE_ROLE_DONGLE
        bool "Dongle (RX from halves)"
endchoice

config KASE_HAS_LOCAL_MATRIX
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n

config KASE_HAS_DISPLAY
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n

config KASE_HAS_BLE
    bool
    default y if KASE_DEVICE_ROLE_KEYBOARD
    default n

config KASE_HAS_RF_RX
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default n

config KASE_HAS_ESPNOW
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default n
```

Le board variant `kase_dongle` set ces flags via un fichier `sdkconfig.defaults.dongle` mergé par CMake quand `BOARD=kase_dongle`.

### Sdkconfig dongle-specific (`sdkconfig.defaults.dongle`)

- `CONFIG_BT_ENABLED=n` (désactive BT, libère ~80 KB heap)
- `CONFIG_ESP_WIFI_ENABLED=y` (pour ESP-NOW)
- `CONFIG_ESP_WIFI_NVS_ENABLED=n` (pas besoin de persistence WiFi)
- `CONFIG_PM_ENABLE=n` (CPU full speed permanent)
- `CONFIG_FREERTOS_HZ=1000` (tick 1 ms — match les budgets latence)
- `CONFIG_TINYUSB_HID_COUNT=1`, `CONFIG_TINYUSB_CDC_COUNT=1`
- `CONFIG_LWIP_*` désactivé au max (pas de TCP/IP)

### Pas d'impact sur V1/V2/V2D

Les flags par défaut sont ceux de `KEYBOARD`, leurs sdkconfig restent inchangés. Test post-merge : compiler les 4 boards consécutivement et vérifier que les 3 anciens binaires sont identiques aux pre-branch (`sha256sum`).

### Branche & CI

- Branche : `dongle-firmware`. PR vers `main` quand stable.
- CI (`.gitlab-ci.yml`) : ajout d'un job `build:kase_dongle` en parallèle des 3 existants.
- Release pipeline : à terme, 8 binaires par release (v1/v2/v2d × app+full → +dongle × app+full).

---

## 11. Safe boot & gestion d'erreurs

### Safe boot

Pattern KaSe_firmware conservé (`BOOT_CRASH_MAGIC` en RTC mem, ≥3 boots consécutifs sans clean shutdown → safe mode).

**En safe mode sur dongle** :
- Skip init NRF (les 2 chips restent reset)
- Skip init ESP-NOW / WiFi
- Skip load NVS keymap/macros/stats (NVS jamais erased)
- TinyUSB minimal : seulement CDC active, HID désactivé
- CDC binaire répond aux commandes standard + permet OTA + permet reset NVS si demandé explicitement
- Recovery via controller software : "rollback OTA" ou "reset RF config" et reboot clean

**Boot crash counter** : reset à 0 après 30 s d'uptime sans crash.

### Erreurs RF

| Condition | Action | Notification user |
|---|---|---|
| NRF SPI init fail (un des 2 chips) | Continue avec l'autre, log error | `KS_CMD_RF_LINK_STATUS` → `link=ERR_HW` pour le half affecté |
| Heartbeat timeout >250 ms | Force-release toutes les touches du half, marque `link=LOST` | Idem |
| Heartbeat reprend | Re-sync state via prochain bitmap, `link=OK` | Idem |
| RX FIFO overflow | Flush FIFO, log + counter, continue | Visible dans `KS_CMD_DONGLE_STATS` |
| Packet CRC error (NRF natif) | Drop silencieux | — |
| Sequence rollback (seq < expected) | Drop comme dup | — |
| Packet type unknown | Drop + log | Counter |

### Erreurs USB

| Condition | Action |
|---|---|
| HID queue full (host ne poll pas) | Bloque `process_key_event` côté engine avec `pdMS_TO_TICKS(5)` timeout. Drop event si timeout (rare en pratique) |
| USB suspend (host endormi) | TinyUSB gère, on stoppe les TX HID. Au resume, état resync via bitmap heartbeat |
| CDC RX overflow (controller flood) | Drop + nack via KR error |

### Erreurs ESP-NOW (cold path)

| Condition | Action |
|---|---|
| Init ESP-NOW fail | Abort opération, KR `ERR_INVAL`, désactive WiFi |
| Peer unreachable (3 retries no ack) | Abort, KR `ERR_LINK_DOWN`, ré-active NRF normal |
| OTA chunk loss / CRC fail | Re-send 3× max, sinon abort. Half a son propre rollback |
| Coexistence dégrade NRF link sévèrement (heartbeat L+R timeout pendant ESP-NOW) | Log warning, continue, track misses dans `DONGLE_STATS` |

### Watchdog

- TWDT sur `rf_rx_task`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` (prod), 2 s (debug).
- Pas de TWDT sur `cdc_task`.

### Logs

- Niveau prod : `ESP_LOG_WARN`.
- Aucun `ESP_LOGI` dans le hot path RF/engine — uniquement `ESP_LOGD` compilé out si `ESP_LOG_LEVEL_NONE`.
- `cdc_task` peut logger en INFO sans impact latence.

---

## 12. Stratégie de test

### Tests host-side (CMake standalone, dans `test/`)

Pareil pattern que firmware actuel.

| Module | Tests | Type |
|---|---|---|
| `rf_packet.h` | encode/decode KEY/HEARTBEAT/TRACKPAD, edge cases (row=15, col=15, seq wraparound) | Pure function |
| `heartbeat.c` | Reconciliation : bitmap diff produit les bons release/press, timeout produit release-all, dup seq ignored | Pure logic, mock du temps |
| `cdc_dongle_cmds.c` | Validation payload, NVS persistence (mock), state transitions (NORMAL→OTA→NORMAL) | Mock NVS, mock RF driver |
| Engine avec `input_source` | Press half_L pos X + press half_R pos Y → HID report contient les deux. Combo cross-half. | Réutilise les tests engine + cas multi-source |
| `to_global_pos()` | Mapping {src, row, col} → pos globale, exhaustif sur 70 positions | Pure function |

### Pas testable host-side

- Coexistence radio NRF/ESP-NOW (besoin 2 dongle + 2 halves réels)
- Latence end-to-end (oscilloscope sur switch + USB analyzer ou LED-trigger)
- USB descriptor validation (`lsusb -v`, USBlyzer, USBPcap)

### Bench tests post-implémentation

1. **RF range** : distance dongle↔half avec halves sur batterie, link_quality stable, cible >5 m ligne directe.
2. **Latence end-to-end** : oscilloscope sur GPIO switch half + USB analyzer. Cible 1 ms typique, 3 ms p99.
3. **Coexistence WiFi domestique** : routeur WiFi 2.4 GHz ch 6 actif → mesurer dégradation link_quality NRF (canaux 76/82 sont au-dessus de WiFi ch 11).
4. **Coexistence ESP-NOW pendant OTA** : trigger OTA half_L, taper sur half_R en continu, vérifier events R arrivent (peuvent avoir +1 ms latence acceptable).
5. **Storm test** : 2 halves simultanément à pleine vitesse, zero key drop, zero stuck.
6. **Sleep/wake host** : laptop sleep → wake → resync propre.
7. **Half power-cycle** : retirer batterie d'un half pendant frappe → release auto + recovery au rebranchement.

### Test infra supplémentaire

- Script Python (`pyserial` + binaire CDC) pour driver le dongle en CI. Réutilise potentiellement la lib du KaSe_controller.
- Optionnel Phase 2 : émulateur half (autre ESP32-S3 + NRF24, firmware "fake half") pour régression latence sans hardware final.

### Build matrix CI

Compiler v1, v2, v2d, **dongle** à chaque push. Vérifier qu'aucun board ne casse.

---

## 13. Hors scope v1 & roadmap framework

### Hors scope v1 explicite (NE PAS faire dans cette branche)

| Sujet | Raison | Renvoyé à |
|---|---|---|
| Firmware halves | Brainstorm séparé | Phase 2, branche dédiée |
| Tamagotchi sur dongle | Pas d'écran sur dongle. Renaîtra sur half-display via push ESP-NOW | Phase 2/3 |
| Push dongle→half pour layer state e-ink | E-ink halves restent autonomes en v1 | Phase 2 |
| Runtime button pairing | Statique-via-NVS suffit pour single-user | Phase 2+ |
| Frequency hopping NRF | Pas observé d'interférence catastrophique en bench | Phase 2 si besoin |
| Test infra "fake half" emulator | Nice to have, pas bloquant | Phase 2 |
| Migration engine en component ESP-IDF | Étape framework | Phase 3 |
| Boot keyboard / bootloader compat | Pas d'utilité M.2 / WWAN slot | Jamais |
| LED Caps Lock vers half | Demande push channel | Phase 2 |
| HID raw / vendor pages | CDC binaire suffit | Si OS pose problème CDC |

### Roadmap framework (north star, non engageant)

```
Phase 1 (cette branche, v4.0)        ← on est là
  └─ Dongle marche, board kase_dongle, refactor minimal (input_source, Kconfig flags)
  └─ V1/V2/V2D inchangés en surface

Phase 2 (~v4.x)
  ├─ Firmware halves : matrix + NRF TX + ESP-NOW + e-ink/trackpad/batterie
  ├─ Run système split en conditions réelles → identifier bugs/manques framework

Phase 3 (~v5.0)
  ├─ Extraction kase_engine en component ESP-IDF propre
  ├─ Transport HAL formalisé (USB / BLE / NRF ESB / ESP-NOW = drivers pluggables)
  ├─ Behavior système (style ZMK : tap-hold/combo/etc en plugins déclaratifs)
  ├─ Generic device tree / boards.json
  ├─ Doc + exemples pour ajouter un nouveau board sans toucher au core

Phase 4 (~v6.0, ambitieux)
  ├─ Builds out-of-tree pour boards externes
  ├─ CLI de scaffolding "kase init my_board"
  ├─ Communauté : recettes Atreus, Lily58, Corne, etc.
```

### Critères de déclenchement Phase 3

Une des trois conditions :
1. On veut ajouter un 3ème type de device (ex: dongle BLE, dongle USB-C dock).
2. On veut accepter des contributions externes (open-sourcer le framework).
3. Le code engine devient trop pollué de `#ifdef CONFIG_KASE_*` pour rester lisible.

### Décisions v1 qui supportent la roadmap framework

- `input_source_t` : extensible (on peut ajouter `INPUT_SRC_BLE_PERIPHERAL`, etc.).
- Kconfig flags `KASE_HAS_*` : modélisent les capabilities, pas les boards. Phase 3 transformera en device tree.
- Engine sans dépendance directe à matrix_scan / display / BLE → déjà découplé en pratique.
- CDC commands rangées par bloc d'IDs (RF=`0xC0-0xCF`, halves=…) → namespace explicite, facile à factorer.

---

## Annexe A — Récap des décisions de brainstorm

| Q | Décision |
|---|---|
| Où vit le keymap engine ? | Sur le dongle. Halves dumb (envoient events matrice + trackpad + batterie). |
| Flux RF half→dongle ? | Matrix events + trackpad + telemetry batterie (dans heartbeat). |
| Flux dongle→half ? | Aucun en hot path. ESP-NOW utilisé pour cold path (OTA, config, telemetry verbose). |
| Cible latence end-to-end ? | ≤1 ms typique, ≤3 ms p99 (gaming-grade). |
| Modèle pairing ? | Statique via NVS (factory default fallback, set via CDC). |
| Robustesse RF ? | ESB ACK + heartbeat 100 ms avec bitmap state reconciliation. |
| Interfaces USB ? | Composite : HID NKRO + HID mouse + HID consumer + HID system + CDC binaire. |
| OTA halves ? | Via ESP-NOW (relay par dongle), pas via NRF. |
| ESP-NOW dongle utilité ? | OTA halves + config push dongle→halves + telemetry verbose. |
| Combien de canaux NRF ? | 2 (un par half) — élimine contention air, résilience séparée. |
| Approche refactor ? | Branche dans KaSe_firmware, board variant kase_dongle, refactor minimal. North star = framework QMK/ZMK-like (Phase 3+). |

## Annexe B — Fichiers à créer / modifier

### Créés

```
boards/kase_dongle/
├── board.h
├── board_keymap.c
├── board_layout.c
├── board_features.h
└── README.md
sdkconfig.defaults.dongle
main/comm/rf/
├── rf_driver.c
├── rf_driver.h
├── rf_rx_task.c
├── rf_rx_task.h
├── rf_packet.h
├── heartbeat.c
└── heartbeat.h
main/comm/espnow/
├── espnow_task.c
├── espnow_task.h
├── espnow_ota.c
├── espnow_ota.h
├── espnow_cfg.c
└── espnow_cfg.h
main/comm/cdc/cdc_dongle_cmds.c
main/comm/cdc/cdc_dongle_cmds.h
docs/CDC_BINARY_PROTOCOL_DONGLE.md
test/test_rf_packet.c
test/test_heartbeat.c
test/test_to_global_pos.c
test/test_cdc_dongle_cmds.c
.gitlab-ci.yml (job build:kase_dongle ajouté)
```

### Modifiés

```
main/Kconfig.projbuild      // device role + capability flags
main/CMakeLists.txt          // sources conditionnelles
main/input/key_processor.c  // signature process_key_event(input_source_t, ...)
main/input/key_processor.h
main/input/keymap.c          // MAX_MATRIX_KEYS=70 si dongle
main/input/keymap.h
main/input/key_definitions.h
main/comm/hid_transport.c    // simplifié sur dongle (USB only)
main/main.c                  // app_main : branchement role-dependent au boot
docs/CDC_BINARY_PROTOCOL.md  // pointer vers DONGLE.md pour 0xC0-0xCF
```

### Inchangés (réutilisés tels quels sur dongle)

```
main/input/key_features.c, tap_hold.c, tap_dance.c, combo.c, leader.c,
hid_report.c, key_stats.c
main/comm/cdc/cdc_acm_com.c, cdc_binary_protocol.c, cdc_binary_cmds.c, cdc_ota.c
partitions.csv
```

---

**Fin du design.**
