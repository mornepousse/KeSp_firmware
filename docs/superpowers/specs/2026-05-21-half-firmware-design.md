# KaSe Half Firmware — Design (MVP)

**Date** : 2026-05-21
**Branche cible** : `dongle-firmware` (même repo, nouveau board variant)
**Status** : Design — brainstorm validé, prêt pour implémentation
**Hardware** : `~/Documents/PCB-esp/` (PCB reversible, schéma KiCad — source de vérité pinout)
**North star** : half dumb, dongle smart — le half scanne sa matrice et envoie les événements
bruts au dongle qui exécute le keymap engine complet.

---

## 1. Vue d'ensemble & cadrage

Ajouter le firmware pour une **moitié (half) de clavier split** dans le repo `KaSe_firmware`.
Le half est un device ESP32-S3-DevKitC (breakout, contrairement au WROOM-2 soudé du dongle)
équipé d'une matrice 7×5 (35 positions) et d'un NRF24L01+ SPI.

**Rôle** : scanner sa matrice locale et transmettre les événements clavier au dongle via
NRF24L01+ Enhanced ShockBurst. Aucun traitement keymap — le dongle est le cerveau.

**Périmètre MVP strict** :

| In scope | Out of scope (déféré) |
|---|---|
| Scan matrice 5×7 | Batterie / ADC / power management |
| TX NRF24L01+ (PTX mode) | Light-sleep / deep-sleep |
| PKT_KEY press/release | ESP-NOW (OTA, config push) |
| PKT_HEARTBEAT 100 ms | E-ink display |
| Retry applicatif unique | Trackpad I2C |
| UART console (debug) | BLE / USB HID |
| Safe boot pattern | Pairing runtime |

**Stratégie repo** : même branche `dongle-firmware`, mêmes fichiers CMake/Kconfig —
on ajoute un 3ème rôle `KASE_DEVICE_ROLE_HALF` et deux nouveaux board variants.

---

## 2. Hardware & pinout

### 2.1 Module MCU

**ESP32-S3-DevKitC** (non-soudé, sur header). Même module que les boards V1/V2/V2D du
firmware keyboard existant. Alim 3.3V depuis le buck DD4012SA (voir PCB-esp/CLAUDE.md).

### 2.2 Matrice clavier

**7 colonnes × 5 lignes = 35 positions** (33 utilisées). Scan COL2ROW comme V2.
Source de vérité : `PCB-esp/CLAUDE.md` ligne "Key matrix".

| Signal | GPIO |
|---|---|
| COL0 | GPIO 8 |
| COL1 | GPIO 7 |
| COL2 | GPIO 39 |
| COL3 | GPIO 5 |
| COL4 | GPIO 2 |
| COL5 | GPIO 42 |
| COL6 | GPIO 4 |
| ROW0 | GPIO 6 |
| ROW1 | GPIO 37 |
| ROW2 | GPIO 3 |
| ROW3 | GPIO 9 |
| ROW4 | GPIO 10 |

**Points d'attention GPIO (voir Section 8)** :
- GPIO 39 (COL2) et GPIO 42 (COL5) sont des pins JTAG. Safe car le DevKitC utilise
  USB-JTAG intégré (pas JTAG externe) et `matrix_setup()` appelle `gpio_reset_pin()`.
  Note explicite dans `PCB-esp/CLAUDE.md` : "GPIO39/42 (JTAG pins) used for COL2/COL5 —
  safe because DevKitC uses USB-JTAG (GPIO3 pull-up)".
- GPIO 3 (ROW2) est JTAG_TDI en plus d'être utilisé pour ROW2. Même justification.

### 2.3 NRF24L01+ (PTX)

Bus SPI partagé (avec 100Ω série sur chaque signal) :

| Signal | GPIO |
|---|---|
| MOSI | GPIO 48 |
| MISO | GPIO 47 |
| SCK | GPIO 45 |
| CSN | GPIO 35 |
| CE | GPIO 36 |
| IRQ | GPIO 21 |

**Point d'attention GPIO 45 (SCK)** : pin strapping `VDD_SPI`. Safe en SPI mode 0
(CPOL=0 = SCLK low à l'idle), ce qui correspond au niveau bas attendu au boot. Noté
explicitement dans `PCB-esp/CLAUDE.md`.

SPI host : `SPI2_HOST`. Clock : 10 MHz (max datasheet NRF24L01+).

En mode PTX, l'IRQ sert à détecter TX_DS (succès) ou MAX_RT (échec après ARC=3).
Le MVP utilise un polling STATUS plutôt que l'ISR pour simplifier (voir Section 5).

### 2.4 Périphériques présents mais non utilisés en MVP

Documenter leur présence matérielle pour le bring-up sans les activer :

| Périphérique | GPIO(s) | Rôle futur |
|---|---|---|
| E-ink display (WeAct SSD1681) | CS=GPIO18, DC=GPIO12, RES=GPIO17, BUSY=GPIO1 | Affichage layer state, firmware version |
| Trackpad TPS43-201A-S (I2C) | SDA=GPIO40, SCL=GPIO38, RST=GPIO13, RDY=GPIO14 | Curseur souris |
| Battery ADC | GPIO15 (ADC2_CH4) | Niveau batterie → heartbeat batt_dV |
| Battery switchable GND | GPIO16 | Divider résistif mesure batterie |
| BMS status | GPIO46 (input only) | État charge LX_LISC_V2 |
| LED backlight | GPIO11 | LED_COM (circuit TPS61040DBV) |

Ces GPIOs ne sont PAS configurés au boot MVP — ils restent en état reset (input flottant).
Ne pas les initialiser en output évite les conflits avec le hardware passif connecté.

---

## 3. Device role & board variants

### 3.1 Nouveau rôle Kconfig

Ajouter `KASE_DEVICE_ROLE_HALF` comme 3ème choix dans `main/Kconfig.projbuild` :

```kconfig
config KASE_DEVICE_ROLE_HALF
    bool "Half (TX to dongle via NRF24)"
```

Capability flags associés au rôle HALF :

| Flag | Valeur HALF | Commentaire |
|---|---|---|
| `KASE_HAS_LOCAL_MATRIX` | `y` | Scan matrice 5×7 propre |
| `KASE_HAS_RF_TX` | `y` | **Nouveau flag** — compile le chemin PTX |
| `KASE_HAS_RF_RX` | `n` | Pas de réception RF |
| `KASE_HAS_DISPLAY` | `n` | |
| `KASE_HAS_BLE` | `n` | |
| `KASE_HAS_TAMA` | `n` | |
| `KASE_HAS_ESPNOW` | `n` | |

Le flag `KASE_HAS_RF_TX` est nouveau (absent des rôles KEYBOARD et DONGLE). Il conditionne
la compilation de `rf_driver_init_tx()` et `rf_driver_send()` dans `rf_driver.c`
(voir Section 5).

### 3.2 Board variants

Deux variants : `kase_half_left` et `kase_half_right`. Le PCB est physiquement identique
(reversible) — ils ne diffèrent que par le `SIDE` define qui sélectionne l'adresse et le
canal NRF.

**Fichiers créés** :

```
boards/kase_half_left/
├── board.h           // SIDE=LEFT, NRF addr suffix 0x01, channel 0x4C
└── board_keymap.c    // keymap placeholder (pas d'engine, mais structure requise)

boards/kase_half_right/
├── board.h           // #include "../kase_half_left/board.h" + overrides SIDE/addr/ch
└── board_keymap.c    // idem
```

Pattern d'héritage : `kase_half_right` inclut `kase_half_left/board.h` puis redéfinit
les macros SIDE/NRF — exactement comme `kase_v2_debug` hérite de `kase_v2` (documenté
dans le `CLAUDE.md` projet : "V2D inherit de V2 via `#include "../kase_v2/board.h"`").

**Contenu `boards/kase_half_left/board.h`** (extraits principaux) :

```c
#define HALF_SIDE_LEFT          1
#define HALF_SIDE               HALF_SIDE_LEFT

/* Matrix 7 cols x 5 rows — COL2ROW, same driver as V2 */
#define MATRIX_ROWS             5
#define MATRIX_COLS             7
#define MAX_MATRIX_KEYS         (MATRIX_ROWS * MATRIX_COLS)   /* 35 */

#define COLS0  GPIO_NUM_8
#define COLS1  GPIO_NUM_7
#define COLS2  GPIO_NUM_39
#define COLS3  GPIO_NUM_5
#define COLS4  GPIO_NUM_2
#define COLS5  GPIO_NUM_42
#define COLS6  GPIO_NUM_4
#define ROWS0  GPIO_NUM_6
#define ROWS1  GPIO_NUM_37
#define ROWS2  GPIO_NUM_3
#define ROWS3  GPIO_NUM_9
#define ROWS4  GPIO_NUM_10

/* NRF24 PTX */
#define BOARD_NRF_SPI_HOST        SPI2_HOST
#define BOARD_NRF_SPI_MOSI        GPIO_NUM_48
#define BOARD_NRF_SPI_MISO        GPIO_NUM_47
#define BOARD_NRF_SPI_SCK         GPIO_NUM_45
#define BOARD_NRF_SPI_CLOCK_HZ    (10 * 1000 * 1000)

#define BOARD_NRF_CSN_GPIO        GPIO_NUM_35
#define BOARD_NRF_CE_GPIO         GPIO_NUM_36
#define BOARD_NRF_IRQ_GPIO        GPIO_NUM_21

/* RF addressing — left half */
#define BOARD_NRF_ADDR_SUFFIX     0x01
#define BOARD_NRF_CHANNEL         0x4C   /* 2476 MHz */
```

**Contenu `boards/kase_half_right/board.h`** :

```c
#include "../kase_half_left/board.h"   /* inherit all */

#undef  HALF_SIDE_LEFT
#undef  HALF_SIDE
#define HALF_SIDE_RIGHT         1
#define HALF_SIDE               HALF_SIDE_RIGHT

#undef  BOARD_NRF_ADDR_SUFFIX
#undef  BOARD_NRF_CHANNEL
#define BOARD_NRF_ADDR_SUFFIX   0x02
#define BOARD_NRF_CHANNEL       0x52   /* 2482 MHz */
```

---

## 4. Architecture & tâches FreeRTOS

### 4.1 Vue d'ensemble

Le half MVP tourne avec **une seule tâche applicative** (`half_scan_task`) plus les
tâches internes TinyUSB (USB désactivé en HID, mais l'USB-Serial reste accessible pour
flash/console).

```
                ┌──────────────────────────────┐
                │      ESP32-S3 (half)          │
                │                              │
Key matrix      │  ┌───────────────────────┐   │   2.4GHz NRF24
  ──── COL/ROW ─►  │   half_scan_task       │──►│──────────────► dongle
                │  │                       │   │   PKT_KEY
                │  │  keyboard_button cb   │   │   PKT_HEARTBEAT
                │  │  heartbeat timer 100ms│   │
                │  └───────────────────────┘   │
                │                              │
                │  (UART console = actif)       │
                └──────────────────────────────┘
```

### 4.2 Tâches

| Tâche | Prio | Stack | Core | Rôle |
|---|---|---|---|---|
| `half_scan_task` | 10 | 4 KB | 0 | Init NRF PTX + keyboard_button component, boucle événements |
| `esp_timer` (interne) | OS | — | — | Heartbeat 100 ms → `heartbeat_timer_cb` |

Pas de `rf_rx_task`, pas de `cdc_task`, pas de `stats_task` sur le half.

### 4.3 Séquence de boot (`main.c` — branche HALF)

```c
// main.c — rôle HALF
app_main():
    safe_boot_check()                      // pattern RTC BOOT_CRASH_MAGIC existant
    if (safe_mode) → log + suspend (HW accessible pour flash)
    gpio_reset_pin() sur tous les GPIO matrice (détacher bootloader/UART0)
    rf_driver_init_tx(&s_radio, &board_nrf_cfg())   // PTX mode
    half_scan_task_start()
```

Safe mode sur half : pas de NRF init, pas de scan matrice. USB-Serial reste accessible.
NVS non effacée. Identique au pattern keyboard existant.

### 4.4 `half_scan_task` — description

```
half_scan_task():
    keyboard_button_init(matrix_cfg)    // même driver composant que matrix_scan.c
    register_callback(on_key_event)
    esp_timer_create(heartbeat_cb, 100ms periodic)
    esp_timer_start_periodic(hb_timer)

    loop forever:
        vTaskDelay(portMAX_DELAY)       // event-driven via callbacks
```

Le composant `keyboard_button` (espressif, déjà dans `components/`) est utilisé comme
driver de matrice bas niveau — les callbacks press/release sont fournis par le half,
non par le keymap engine. L'engine n'est **pas compilé** sur le half.

**Callback press/release** :

```c
static void on_key_event(uint8_t row, uint8_t col, bool pressed)
{
    rf_key_event_t e = {
        .row      = row,
        .col      = col,
        .pressed  = pressed,
        .is_retry = false,
        .seq      = s_seq++,    // rolling 8-bit, post-increment
    };
    uint8_t buf[3];
    rf_encode_key(buf, &e);
    bool ok = rf_driver_send(&s_radio, buf, 3);
    if (!ok) {
        s_pending_retry = e;
        s_pending_retry.is_retry = true;
        s_has_pending_retry = true;
    }
    // update local pressed bitmap for heartbeat
    rf_bitmap_set(s_pressed_bitmap, row, col, pressed);
}
```

**Heartbeat timer callback** :

```c
static void heartbeat_timer_cb(void *arg)
{
    // retry unique : tenter une retransmission de la dernière erreur
    if (s_has_pending_retry) {
        uint8_t buf[3];
        rf_encode_key(buf, &s_pending_retry);
        rf_driver_send(&s_radio, buf, 3);   // best-effort, pas de 2ème retry
        s_has_pending_retry = false;
    }

    rf_heartbeat_t hb = {
        .batt_dV = 0,           // MVP : batterie non mesurée
        .link_q  = s_max_rt_count,  // nb MAX_RT depuis dernier heartbeat
        .seq     = s_seq++,
    };
    memcpy(hb.bitmap, s_pressed_bitmap, RF_HALF_BITMAP_BYTES);

    uint8_t buf[9];
    rf_encode_heartbeat(buf, &hb);
    rf_driver_send(&s_radio, buf, 9);

    s_max_rt_count = 0;   // reset compteur link_q
}
```

**Séquence numéro de séquence** : un compteur 8-bit global `s_seq` incrémenté à chaque
paquet transmis (PKT_KEY et PKT_HEARTBEAT partagent le même compteur par half). Wrap
naturel de 0xFF à 0x00 — le dongle gère le wrap dans `hb_apply_key()` existant.

---

## 5. Généralisation du rf_driver (chemin PTX)

### 5.1 Principe

Le `rf_driver.c` actuel implémente uniquement le mode PRX (réception dongle). On l'étend
de façon **rétro-compatible** : l'API existante reste inchangée pour le dongle, on ajoute
deux nouvelles fonctions derrière le flag `CONFIG_KASE_HAS_RF_TX`.

### 5.2 Nouvelles fonctions dans `rf_driver.h`

```c
/* Initialize the radio in PTX mode (transmitter).
 * Sets TX_ADDR + RX_ADDR_P0 to the same 5-byte address (required for auto-ACK),
 * channel, 2 Mbps, ESB + DPL, ARC=3, ARD=500µs.
 * shares_bus_first=true → initializes the SPI bus.
 * Returns ESP_OK on success; sets radio->present. */
esp_err_t rf_driver_init_tx(rf_radio_t *radio, const rf_radio_cfg_t *cfg);

/* Transmit one payload (PTX).
 * Writes W_TX_PAYLOAD, pulses CE high ~15µs, polls STATUS for TX_DS (success)
 * or MAX_RT (no ACK after ARC=3 retries). Clears IRQ flags. Returns true on ACK.
 * Timeout ~4 ms (ARC=3 x ARD=500µs x 2 + margin). Polled — no TX IRQ needed. */
bool rf_driver_send(rf_radio_t *radio, const uint8_t *buf, uint8_t len);
```

### 5.3 `rf_driver_init_tx()` — séquence registres NRF24L01+

La séquence est symétrique à `rf_driver_init()` (PRX) avec deux différences :
`PRIM_RX=0` dans CONFIG et écriture des adresses TX_ADDR + RX_ADDR_P0 au lieu de RX_ADDR_P0
seul.

```c
esp_err_t rf_driver_init_tx(rf_radio_t *r, const rf_radio_cfg_t *cfg)
{
    // [SPI bus init + GPIO CSN/CE/IRQ setup : identique à rf_driver_init]
    // ...

    rf_driver_write_reg(r, REG_CONFIG, 0x00);          // power down
    rf_driver_write_reg(r, REG_EN_AA,  0x01);          // auto-ack pipe 0
    rf_driver_write_reg(r, REG_EN_RXADDR, 0x01);       // pipe 0 pour ACK reception
    rf_driver_write_reg(r, REG_SETUP_AW, 0x03);        // 5-byte address
    rf_driver_write_reg(r, REG_SETUP_RETR, 0x13);      // ARD=500µs, ARC=3
    rf_driver_set_channel(r, cfg->channel);
    rf_driver_write_reg(r, REG_RF_SETUP, 0x0E);        // 2 Mbps, 0 dBm
    rf_driver_write_reg(r, REG_FEATURE, 0x04);         // EN_DPL
    rf_driver_write_reg(r, REG_DYNPD, 0x01);           // DPL pipe 0

    uint8_t addr[5];
    memcpy(addr, cfg->rx_addr, 4);
    addr[4] = cfg->addr_suffix;
    write_reg_buf(r, REG_TX_ADDR,    addr, 5);   // adresse du destinataire (dongle)
    write_reg_buf(r, REG_RX_ADDR_P0, addr, 5);   // DOIT matcher TX_ADDR pour ACK ESB

    rf_driver_write_reg(r, REG_STATUS, 0x70);          // clear RX_DR|TX_DS|MAX_RT
    // flush TX FIFO
    { uint8_t c = CMD_FLUSH_TX, rx; csn_low(r); spi_xfer(r, &c, &rx, 1); csn_high(r); }

    // PTX mode : PRIM_RX=0, EN_CRC|CRCO|PWR_UP, mask RX_DR IRQ (pas utile en PTX)
    // CONFIG = 0x3E : MASK_RX_DR=1, MASK_TX_DS=0, MASK_MAX_RT=0, EN_CRC=1, CRCO=1, PWR_UP=1, PRIM_RX=0
    rf_driver_write_reg(r, REG_CONFIG, 0x3E);
    vTaskDelay(pdMS_TO_TICKS(2));           // Tpd2stby = 1.5ms (datasheet)
    // CE reste LOW en PTX — pulsé uniquement lors de chaque envoi

    r->present = true;
    ESP_LOGI(TAG, "radio PTX ch=%u addr=KaSe.%02x init OK", cfg->channel, cfg->addr_suffix);
    return ESP_OK;
}
```

**Note registre CONFIG=0x3E** : bits en PTX mode :
- `MASK_RX_DR=1` : masque l'IRQ RX (inutile en PTX, évite confusion)
- `MASK_TX_DS=0` : IRQ TX_DS activée (succès ACK)
- `MASK_MAX_RT=0` : IRQ MAX_RT activée (échec)
- `EN_CRC=1, CRCO=1` : CRC 2 octets
- `PWR_UP=1, PRIM_RX=0` : mode PTX

Le MVP n'utilise pas l'IRQ NRF en TX (polling STATUS) — les masques IRQ n'influencent
que la broche IRQ physique, pas les bits STATUS.

### 5.4 `rf_driver_send()` — séquence envoi polled

```c
bool rf_driver_send(rf_radio_t *r, const uint8_t *buf, uint8_t len)
{
    // W_TX_PAYLOAD : écrit le payload dans le FIFO TX
    uint8_t tx[33], rx[33];
    tx[0] = CMD_W_TX_PAYLOAD;   // 0xA0
    memcpy(&tx[1], buf, len);
    csn_low(r); spi_xfer(r, tx, rx, len + 1); csn_high(r);

    // Pulse CE high ≥10µs pour déclencher la transmission
    ce_high(r);
    esp_rom_delay_us(15);        // 15µs > minimum 10µs datasheet
    ce_low(r);

    // Poll STATUS jusqu'à TX_DS (bit5) ou MAX_RT (bit4)
    // Timeout : ARC=3 × ARD=500µs × 2 ≈ 3ms + margin → 5ms
    uint32_t deadline = esp_timer_get_time() + 5000;  // µs
    uint8_t status;
    do {
        status = rf_driver_read_reg(r, REG_STATUS);
        if (status & 0x30) break;   // TX_DS ou MAX_RT
    } while (esp_timer_get_time() < deadline);

    bool success = (status & 0x20) != 0;   // bit5 TX_DS

    if (status & 0x10) {   // MAX_RT
        // vider le TX FIFO (sinon le packet reste et bloque les suivants)
        uint8_t c = CMD_FLUSH_TX;
        csn_low(r); spi_xfer(r, &c, &rx[0], 1); csn_high(r);
        r->pkt_dup++;   // recycler le compteur comme "tx_fail"
    }

    // Clear TX_DS + MAX_RT dans STATUS
    rf_driver_write_reg(r, REG_STATUS, 0x30);

    if (success) r->pkt_rx++;   // recycler comme "pkt_tx_ok"
    else s_max_rt_count++;       // static dans rf_driver.c ou exposé en extern
    return success;
}
```

**Nouveaux registres requis** (ajouter dans rf_driver.c) :

| Registre | Adresse | Usage |
|---|---|---|
| `REG_TX_ADDR` | `0x10` | Adresse du destinataire PTX (5 bytes) |
| `CMD_W_TX_PAYLOAD` | `0xA0` | Écrire payload dans FIFO TX |
| `CMD_FLUSH_TX` | `0xE1` | Vider FIFO TX après MAX_RT |

### 5.5 Rétro-compatibilité dongle

Le dongle continue d'appeler `rf_driver_init()` (PRX) — inchangé. Les nouvelles fonctions
`rf_driver_init_tx` et `rf_driver_send` sont conditionnées par `#if CONFIG_KASE_HAS_RF_TX`
pour ne pas alourdir la compilation dongle.

---

## 6. Flux de données & robustesse

### 6.1 Chemin hot path (press/release)

```
Key physique actionnée
  → keyboard_button callback (ISR-safe, depuis task keyboard_button)
  → on_key_event(row, col, pressed)
  → rf_encode_key() → buf[3]
  → rf_driver_send() : W_TX_PAYLOAD + pulse CE + poll STATUS
      Si TX_DS (ACK reçu) → s_max_rt_count inchangé, bitmap mis à jour
      Si MAX_RT (3 retries ESB épuisés) → s_has_pending_retry=true, s_max_rt_count++
```

Budget latence half-side :
- `keyboard_button` debounce : ~5 ms (configurable, voir `BOARD_DEBOUNCE_TICKS`)
- `rf_encode_key` : <1 µs
- `rf_driver_send` happy path : ~350 µs (ESB 1 retry max, 2 Mbps, 3B payload)
- `rf_driver_send` MAX_RT (3 retries) : ~4 ms (3×500µs ARD + on-air + timeouts)

Total happy path half → NRF TX : ~5.35 ms (dominé par debounce).

### 6.2 Retry applicatif (best-effort)

Si `rf_driver_send` retourne `false` (MAX_RT) :
1. L'événement est mis de côté dans `s_pending_retry` avec `is_retry=true`.
2. Au prochain tick heartbeat (≤100 ms), le heartbeat_timer_cb tente une
   retransmission unique (pas de 2ème retry si celui-ci échoue aussi).
3. Le dongle détecte la divergence via le bitmap heartbeat et force-press/release
   les touches manquantes — le bitmap est la source de vérité finale.

Ce schéma est intentionnellement simple : la reconciliation dongle (`heartbeat.c`,
Plan 2 déjà implémenté) est le filet de sécurité. La retransmission applicatif réduit
la latence de correction de 100 ms à ~0 ms dans le cas courant.

### 6.3 Heartbeat (anti-stuck safety net)

Période : 100 ms (timer ESP32 hardware via `esp_timer`).

Contenu `PKT_HEARTBEAT` (9 bytes, codec `rf_packet.h`) :

| Champ | Valeur MVP | Notes |
|---|---|---|
| `bitmap` | 5 bytes MSB-first | row×7+col, état courant des 35 touches |
| `batt_dV` | `0` | Batterie non mesurée en MVP |
| `link_q` | `s_max_rt_count` | Nb MAX_RT depuis le dernier HB |
| `seq` | `s_seq++` | Compteur 8-bit partagé |

Le dongle compare le bitmap reçu à son état local (`hb_reconcile()`) et force les
press/release manquants. Logique existante dans `heartbeat.c` — le half ne connaît
pas ce mécanisme, il envoie juste son état réel.

### 6.4 Coordonnées locales vs globales

Le half envoie **toujours ses coordonnées locales** (row 0..4, col 0..6).
Le dongle effectue le mapping vers les coordonnées globales :

```
half_left  : global_col = local_col          (0..6)
half_right : global_col = local_col + 7      (7..13)
```

Défini dans `rf_rx_task.c` (existant) via `HALF_R_COL_OFFSET=7` et `on_force_press/
on_force_release` callbacks. Le half n'a pas besoin de connaître ce mapping.

### 6.5 Bitmap local

`s_pressed_bitmap[RF_HALF_BITMAP_BYTES]` maintenu dans `half_scan_task.c` :
- Set à `true` sur chaque press dans `on_key_event()`.
- Set à `false` sur chaque release dans `on_key_event()`.
- Copié dans `PKT_HEARTBEAT.bitmap` à chaque tick.
- Helpers : `rf_bitmap_set()` / `rf_bitmap_get()` depuis `rf_packet.h` (partagé).

---

## 7. Stratégie de test

### 7.1 Tests host-side (nouveau)

Pattern identique à `test/test_rf_packet.c` et `test/test_heartbeat.c` (Plan 2).

**`test/test_half_matrix_diff.c`** : test de la logique pure "previous pressed set →
current pressed set → liste de PKT_KEY générés" :

```c
// Cas : 2 touches pressées, puis 1 relâchée, puis 1 nouvellement pressée
// → vérifier que les bons PKT_KEY press/release sont émis avec les bons row/col
```

Cette logique est séparable du driver matrice — elle opère sur deux bitmaps (avant/après)
et produit la liste des changements. Testable sans hardware.

Les tests rf_packet (encode/decode KEY/HEARTBEAT) sont déjà écrits et passent — le half
utilise le même codec.

### 7.2 Validation bench hardware

Ordre recommandé :

1. **Flash + console** : `idf.py -B build_half_left -p /dev/ttyUSB1 flash monitor`
   Vérifier les logs boot : probe NRF (`CONFIG=0x... RF_SETUP=0x... -> OK`), init matrice.

2. **RF link** : brancher le dongle, observer ses logs `rf_rx`:
   - `pkt_rx` incrémente à chaque PKT_KEY envoyé par le half
   - `PKT_HEARTBEAT` reçu toutes les ~100 ms
   - Aucune divergence bitmap (link_q=0, pas de force-press/release dongle-side)

3. **Frappe réelle** : presser une touche sur le half → caractère visible sur le PC
   (via dongle USB HID). Tester press+release + plusieurs touches simultanées.

4. **Robustesse** : débrancher le half pendant frappe → dongle fait force-release (timeout
   250 ms heartbeat) → aucune touche stuck. Rebrancher → link reprend au prochain heartbeat.

5. **Deux halves** : flash `kase_half_right` sur un second DevKitC (même canal/adresse
   distincts : 0x52 / suffix 0x02) → frappe simultanée sur les deux halves, zéro stuck.

### 7.3 Outil de flash

Flash via CP210x (USB-Serial intégré au DevKitC) sur `/dev/ttyUSB1` (ou ttyUSB0 selon
la numérotation du système) :

```bash
idf.py -B build_half_left -p /dev/ttyUSB1 flash monitor
```

Le dongle est flashé via CH340C sur `/dev/ttyUSB0`.

---

## 8. Build system

### 8.1 Commandes de build

```bash
source ~/esp/esp-idf/export.sh

# IMPORTANT : sdkconfig est partagé entre tous les boards
# Supprimer avant de switcher de rôle (KEYBOARD ↔ DONGLE ↔ HALF)
rm -f sdkconfig

# Half gauche
idf.py -B build_half_left  -DBOARD=kase_half_left  -DIDF_TARGET=esp32s3 build
idf.py -B build_half_left  -p /dev/ttyUSB1 flash

# Half droit
idf.py -B build_half_right -DBOARD=kase_half_right -DIDF_TARGET=esp32s3 build
idf.py -B build_half_right -p /dev/ttyUSB1 flash
```

Le dossier build est dédié par board (`build_half_left/`, `build_half_right/`) pour
éviter les recompilations complètes inter-board.

### 8.2 sdkconfig spécifique HALF (`sdkconfig.defaults.half`)

```ini
# Console UART activée (contrairement au dongle qui a CONSOLE_NONE)
# GPIO43/44 libres pour la console sur half (pas d'usage matrice sur ces pins)
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# Pas de BT sur half MVP
CONFIG_BT_ENABLED=n

# Pas de WiFi (ESP-NOW désactivé MVP)
CONFIG_ESP_WIFI_ENABLED=n

# CPU 240 MHz fixe
CONFIG_PM_ENABLE=n
CONFIG_FREERTOS_HZ=1000

# TinyUSB : CDC uniquement (console/flash), pas de HID composite
CONFIG_TINYUSB_CDC_ENABLED=y
CONFIG_TINYUSB_HID_ENABLED=n
```

**Console UART** : le half garde la console UART active (contrairement au dongle qui
utilise `CONFIG_ESP_CONSOLE_NONE=y` pour libérer GPIO43/44 utilisés comme UART0 dans
les boards V2). Sur le half, GPIO43/44 ne sont pas assignés à la matrice — ils peuvent
servir à la console.

### 8.3 CMake — sources conditionnelles

Ajout dans `main/CMakeLists.txt` :

```cmake
if(CONFIG_KASE_HAS_RF_TX)
    list(APPEND srcs "comm/rf/half_scan_task.c")
    # rf_driver.c déjà dans la liste si RF_RX — sinon ajouter :
    if(NOT CONFIG_KASE_HAS_RF_RX)
        list(APPEND srcs "comm/rf/rf_driver.c")
    endif()
endif()
```

`half_scan_task.c` est un nouveau fichier sous `main/comm/rf/` (ou `main/input/` selon
l'organisation retenue à l'implémentation — la localisation est une décision d'implémentation).

Sur le half, `KASE_HAS_LOCAL_MATRIX=y` compile `matrix_scan.c` (setup GPIO pins).
`keyboard_task.c` **ne doit PAS être compilé** sur le half — il appelle directement
`build_keycode_report()` et l'engine. À la place, `half_scan_task.c` s'appuie sur le
composant `keyboard_button` pour les callbacks matrice (même driver bas niveau, sans
l'engine au-dessus).

Conséquence pour `main/CMakeLists.txt` : séparer `matrix_scan.c` (compilé si
`KASE_HAS_LOCAL_MATRIX`) de `keyboard_task.c` (compilé uniquement si
`KASE_DEVICE_ROLE_KEYBOARD`). L'engine (`key_processor.c`, `hid_report.c`, etc.)
et les tâches associées sont également conditionnés à `KASE_DEVICE_ROLE_KEYBOARD`.

---

## 9. Points d'attention & ambiguïtés hardware

### 9.1 GPIO 39 et 42 — JTAG pins (COL2, COL5)

GPIO39 (JTAG_TDI) et GPIO42 (JTAG_TMS) sont utilisés pour COL2 et COL5 de la matrice.

**Statut** : **safe, documenté** dans `PCB-esp/CLAUDE.md` :
> "GPIO39/42 (JTAG pins) used for COL2/COL5 — safe because DevKitC uses USB-JTAG (GPIO3
> pull-up)"

Le DevKitC utilise l'USB-JTAG intégré (basé sur USB OTG, pas sur les pins JTAG physiques).
Les pins 39/42 sont libres pour un usage GPIO. `matrix_setup()` appelle `gpio_reset_pin()`
sur toutes les pins matrice avant configuration.

**Recommandation** : confirmer que le débogage JTAG physique n'est pas requis en
développement half. Si on souhaite un debug pas-à-pas hardware, il faudra rerouter
COL2/COL5 (changement PCB).

### 9.2 GPIO 45 — SCK (strapping pin VDD_SPI)

GPIO45 est `VDD_SPI`, un strapping pin qui influence le niveau de la flash SPI au boot.

**Statut** : **safe, documenté** dans `PCB-esp/CLAUDE.md` :
> "GPIO45/SCK is a strapping pin but safe: pull-down internal + SPI mode 0 = LOW at boot"

SPI mode 0 (CPOL=0) = SCLK LOW à l'état idle. Pendant le boot, GPIO45 est low
(pull-down interne DevKitC). Les 100Ω série protègent contre les niveaux transitoires.

**Recommandation** : à l'implémentation, vérifier que les 100Ω n'impactent pas la
qualité du signal à 10 MHz (montée de front ~50ns max pour NRF24 à cette fréquence).
Si des erreurs SPI apparaissent, réduire à 8 MHz.

### 9.3 GPIO 3 — ROW2 (aussi JTAG_TDI)

GPIO3 est également une pin JTAG (TDI) et est utilisée pour ROW2. Même justification
que GPIO39/42 : safe avec USB-JTAG intégré. Flaggé ici car pas explicitement mentionné
dans `PCB-esp/CLAUDE.md` pour GPIO3.

**Action utilisateur** : confirmer que GPIO3 comme ROW2 ne cause pas de problème au
boot (vérifier la résistance interne et l'état de la ligne ROW en mode input à `matrix_setup`).

### 9.4 GPIO 21 — IRQ NRF (potentiellement partagé)

Sur le dongle, IRQ1=GPIO8 et IRQ2=GPIO2. Sur le half, IRQ=GPIO21. Pas de conflit
interne — GPIO21 est libre sur le half (e-ink BUSY est sur GPIO1, pas GPIO21).

**Vérification** : GPIO21 est `USB_D-` sur l'ESP32-S3 natif. Sur le DevKitC, la
signalisation USB D- passe par le circuit CH340C/USB interne, pas par GPIO21 directement.
À confirmer dans le schéma `PCB-esp/KaSe.kicad_sch` que GPIO21 n'est pas connecté
à la piste USB D- sur le PCB half.

### 9.5 Pas de conflit SPI matrice / NRF

La matrice utilise COL/ROW GPIO discrets (scan digital), pas de SPI. Le bus SPI NRF
(MOSI=48, MISO=47, SCK=45, CSN=35) est indépendant. Pas de conflit.

### 9.6 Adresse NRF et séparation de canaux

Les adresses et canaux sont **distincts et non-modifiables au runtime en MVP**
(compilés dans `board.h`) :

| Half | Canal | Fréquence | Adresse (5B) |
|---|---|---|---|
| Left | `0x4C` (76) | 2476 MHz | `KaSe\x01` |
| Right | `0x52` (82) | 2482 MHz | `KaSe\x02` |

Ces valeurs doivent correspondre aux defaults du dongle (`RF_CH_LEFT_DEFAULT`,
`RF_CH_RIGHT_DEFAULT`, `RF_ADDR_BASE_DEFAULT`) dans `boards/kase_dongle/board_rf.h`.

---

## 10. Hors scope explicite (déféré post-MVP)

| Sujet | Déféré à |
|---|---|
| Batterie ADC (GPIO15/16) → `batt_dV` réel | Phase 2 |
| Light-sleep entre scans (économie énergie) | Phase 2 |
| Deep-sleep avec wake-on-keypress | Phase 2 |
| ESP-NOW : réception OTA + config push du dongle | Phase 2 |
| E-ink display (SSD1681, SPI) | Phase 2 |
| Trackpad TPS43-201A-S (I2C) | Phase 2 |
| Pairing runtime (changement canal/adresse) | Phase 2+ |
| Fréquency hopping NRF | Phase 2 si interférences observées |
| Push dongle→half layer state (e-ink) | Phase 3 |
| BLE HID (bypass dongle) | Phase 3+ |

---

## Annexe A — Récap des décisions de design

| Question | Décision |
|---|---|
| Où vit le keymap engine ? | Dongle uniquement. Half est dumb. |
| Que transmet le half ? | PKT_KEY (press/release local) + PKT_HEARTBEAT (bitmap 100ms) |
| Comment gérer les pertes RF ? | ESB ARC=3 + retry applicatif unique au HB + reconciliation dongle |
| Console UART sur half ? | Oui (debug bring-up). Contrairement au dongle (CONSOLE_NONE). |
| USB HID sur half ? | Non. USB = flash/console uniquement. |
| Deux boards ou un seul ? | Deux variants (left/right), right hérite de left via `#include`. |
| Coordonnées transmises ? | Locales (row 0..4, col 0..6). Mapping global = responsabilité dongle. |
| PTX polling ou IRQ ? | Polling STATUS (MVP, simpler). IRQ TX optionnel en Phase 2. |
| Partage SPI matrice/NRF ? | Non — matrice = GPIO discrets, NRF = SPI dédié. |
| sdkconfig partagé ? | Oui (quirk repo). Toujours `rm -f sdkconfig` avant de changer de board. |

---

## Annexe B — Fichiers à créer / modifier

### Créés

```
boards/kase_half_left/
├── board.h               // pinout matrice + NRF left (GPIO exact, channel 0x4C, suffix 0x01)
└── board_keymap.c        // keymap placeholder (structure requise par le build)

boards/kase_half_right/
├── board.h               // #include kase_half_left/board.h + undef/redef SIDE/addr/ch
└── board_keymap.c        // idem

sdkconfig.defaults.half   // UART console y, BT n, WiFi n, TinyUSB CDC only

main/comm/rf/half_scan_task.c   // task init + on_key_event + heartbeat_timer_cb
main/comm/rf/half_scan_task.h   // API : half_scan_task_start()

test/test_half_matrix_diff.c    // tests host-side logique bitmap diff → PKT_KEY list
```

### Modifiés

```
main/Kconfig.projbuild     // +KASE_DEVICE_ROLE_HALF, +KASE_HAS_RF_TX flag
main/CMakeLists.txt        // if(KASE_HAS_RF_TX) → half_scan_task.c + rf_driver.c
main/comm/rf/rf_driver.h   // +rf_driver_init_tx(), +rf_driver_send()
main/comm/rf/rf_driver.c   // +rf_driver_init_tx() impl, +rf_driver_send() impl,
                           //  +REG_TX_ADDR, +CMD_W_TX_PAYLOAD, +CMD_FLUSH_TX
main/main.c                // app_main : branche KASE_DEVICE_ROLE_HALF → half_scan_task_start()
test/CMakeLists.txt        // +test_half_matrix_diff.c
test/test_main.c           // +RUN_TEST(test_half_matrix_diff)
```

### Inchangés (réutilisés tels quels sur le half)

```
main/comm/rf/rf_packet.h       // codec partagé dongle + half
main/comm/rf/heartbeat.h/c     // logique dongle uniquement — pas utilisée sur half
main/input/matrix_scan.c       // GPIO setup (KASE_HAS_LOCAL_MATRIX=y) — compilé sur half
// main/input/keyboard_task.c → NE PAS compiler sur half (appelle engine)
components/keyboard_button/    // composant espressif existant
partitions.csv                 // table de partitions inchangée
```

---

**Fin du design.**
