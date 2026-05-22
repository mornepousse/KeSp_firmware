# KaSe Half Peripherals + ESP-NOW Info Channel — Design (Bricks/Scaffolding)

**Date** : 2026-05-22
**Branche cible** : `dongle-firmware`
**Status** : Design — prêt pour implémentation skeleton
**Extends** : `2026-05-21-half-firmware-design.md` (MVP validé, Section 10 "hors scope" décrit
exactement le périmètre que ce document spécifie)
**Hardware source of truth** : `~/Documents/PCB-esp/CLAUDE.md`

---

## 0. Niveau de livraison : BRICKS, pas des drivers complets

Ce document spec des **briques scaffolding** — pas des drivers complets. Chaque brick livre :

- Structure de module propre (`periph/trackpad/`, `periph/eink/`, `comm/espnow/`)
- Flags Kconfig corrects, compilation conditionnelle
- API avec signatures réelles, init/probe réelles, intégration câblée
- Stubs clairement marqués `/* TODO: */` pour la logique métier

Les **stubs** sont les parties que l'utilisateur remplira ensuite. Par exemple :
- La lecture des registres IQS5xx par I2C (protocole propriétaire, non documenté ici)
- Les séquences de commandes SSD1681 (init VCOM, LUT, refresh OTP vs custom)
- Le rendu 1bpp sur le framebuffer e-ink
- Les handlers ESP-NOW (corps des callbacks)

**Ce n'est pas négligeable** : les séquences e-ink et le protocole IQS5xx sont la majorité
du code futur. Les bricks ouvrent la voie ; ils ne font pas le travail.

---

## 1. Vue d'ensemble

### 1.1 Contexte

Le MVP half (`2026-05-21-half-firmware-design.md`) est validé et implémenté : scan
matrice + NRF24 PTX + heartbeat fonctionnent. La Section 10 de ce MVP listait comme
"Phase 2" : trackpad I2C, e-ink SSD1681, ESP-NOW push layer/état, batterie ADC.

Ce document spécifie les **3 premières briques Phase 2** :

1. **Trackpad** (half) : IQS5xx via I2C → `rf_trackpad_t` → `PKT_TRACKPAD` → dongle →
   mouse HID (stub côté dongle)
2. **E-ink** (half) : SSD1681 via SPI partagé → framebuffer 1bpp → affichage layer/état
   reçu par ESP-NOW
3. **ESP-NOW info channel** (dongle + half) : canal léger et permanent pour layer state,
   battery level — distinct du canal ESP-NOW OTA/config (Plan 5, réservé)

Ce qui reste **hors scope** de ces bricks (Section 6 ci-dessous) : logique IQS5xx, séquences
SSD1681, rendu, battery ADC, pairing, OTA via ESP-NOW, mouse HID composite.

### 1.2 Philosophie hardware

Le PCB half est **réversible** : les deux halves sont physiquement identiques. L'un peut
porter le trackpad (côté gauche ou droit selon la préférence utilisateur), l'autre le
display e-ink — mais les deux périphériques sont présents sur le PCB. Conséquence :

> Les deux bricks (trackpad + e-ink) **sont compilés sur les deux variants** (`kase_half_left`
> et `kase_half_right`). La présence physique est détectée à l'exécution par probe
> (I2C ACK pour le trackpad, probe SPI/BUSY pour l'e-ink). Le code ne suppose pas que
> seul un half a un périphérique donné.

---

## 2. Structure des modules (nouveaux)

```
main/
├── periph/
│   ├── half_spi.h           # API verrou bus SPI2 partagé NRF + e-ink
│   ├── half_spi.c           # Implémentation : wrapper du s_tx_mutex existant
│   ├── trackpad/
│   │   ├── trackpad.h       # API publique : trackpad_init(), trackpad_start()
│   │   └── trackpad.c       # I2C init, RDY ISR, TX RF — stubs IQS5xx register reads
│   └── eink/
│       ├── eink.h           # API publique : eink_init(), eink_clear(), eink_push(), eink_start()
│       └── eink.c           # SPI device, GPIO, probe, refresh task — stubs SSD1681 cmds + rendu
└── comm/
    └── espnow/
        ├── espnow_link.h    # API publique : espnow_link_init(), espnow_send()
        ├── espnow_link.c    # WiFi STA init, esp_now_init, peer mgmt, send + recv dispatch
        ├── espnow_msg.h     # Type IDs + payload structs des messages info-channel
        └── espnow_info.c    # Handlers info-channel (stubs)
```

### 2.1 Relation avec `half_scan_task.c` (existant)

`half_scan_task.c` possède `s_tx_mutex` (un `SemaphoreHandle_t`) qui sérialise les envois
NRF SPI. Ce mutex est **refactoré** en `half_spi_lock()` / `half_spi_unlock()` exposés par
`half_spi.h` — de sorte que `trackpad.c` et `eink.c` puissent prendre ce même verrou sans
connaître les internals de `half_scan_task`.

`half_scan_task.c` reste le point d'initialisation de l'ensemble : il appelle
`trackpad_init()`, `eink_init()`, `espnow_link_init()` dans sa séquence boot (après NRF
init), puis `trackpad_start()` et `eink_start()` si les périphériques sont présents.

---

## 3. Kconfig (ajouts)

Dans `main/Kconfig.projbuild`, ajouter après `KASE_HAS_RF_TX` :

```kconfig
config KASE_HAS_TRACKPAD
    bool
    default y if KASE_DEVICE_ROLE_HALF
    default n
    help
        Compiles trackpad.c (IQS5xx I2C driver + RF trackpad TX).
        Runtime probe: trackpad_init() returns false if not physically present.
        Compiled on both half variants (reversible PCB).

config KASE_HAS_EINK
    bool
    default y if KASE_DEVICE_ROLE_HALF
    default n
    help
        Compiles eink.c (SSD1681 SPI driver + refresh task) and half_spi.c
        (shared SPI2 bus lock used by both NRF and e-ink paths).
        Runtime probe: eink_init() returns false if not physically present.
        Compiled on both half variants (reversible PCB).
```

`KASE_HAS_ESPNOW` est **étendu** pour inclure le rôle HALF (actuellement `default y if
KASE_DEVICE_ROLE_DONGLE` seulement) :

```kconfig
config KASE_HAS_ESPNOW
    bool
    default y if KASE_DEVICE_ROLE_DONGLE
    default y if KASE_DEVICE_ROLE_HALF     # ← nouveau
    default n
    help
        Compiles ESP-NOW stack.
        On the dongle: info-channel + Plan 5 OTA/config (espnow_link + espnow_info).
        On the half: info-channel only (layer/state RX, battery TX).
        Reserve EN_OTA_* / EN_CFG_* type IDs for Plan 5 (dongle-initiated OTA/config).
```

### 3.1 Implications sdkconfig

`sdkconfig.defaults.half` (existant) contient `CONFIG_ESP_WIFI_ENABLED=n`. Avec
`KASE_HAS_ESPNOW=y` sur HALF, il faut **activer WiFi** sur les halves. Voir Section 5.4
(WiFi delta) pour le détail.

---

## 4. Brick : Verrou SPI partagé (`half_spi`)

### 4.1 Contexte

Le bus SPI2 (`SPI2_HOST`, MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45) est partagé entre :
- NRF24L01+ (CSN=GPIO35) — accédé par `rf_driver_send()` depuis `half_scan_task`
- SSD1681 e-ink (CS_DISP=GPIO18) — accédé par `eink_push()` depuis la tâche e-ink

Le `s_tx_mutex` dans `half_scan_task.c` (MVP) protège déjà les sends NRF. Il doit être
transformé en verrou de bus partagé pour que l'e-ink puisse aussi le prendre.

### 4.2 API (`half_spi.h`)

```c
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Initialize the shared SPI2 bus lock.
 * Must be called once, before trackpad_init() or eink_init().
 * Called internally by half_scan_task before rf_driver_init_tx(). */
void half_spi_lock_init(void);

/* Acquire the SPI2 bus for exclusive use. Blocks until available.
 * Must be called before any spi_device_polling_transmit on SPI2. */
void half_spi_lock(void);

/* Release the SPI2 bus. */
void half_spi_unlock(void);
```

### 4.3 Implémentation (`half_spi.c`)

```c
#include "half_spi.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_spi_mutex = NULL;

void half_spi_lock_init(void) {
    s_spi_mutex = xSemaphoreCreateMutex();
}

void half_spi_lock(void)   { xSemaphoreTake(s_spi_mutex, portMAX_DELAY); }
void half_spi_unlock(void) { xSemaphoreGive(s_spi_mutex); }
```

**Refactoring `half_scan_task.c`** : remplacer `s_tx_mutex` par des appels
`half_spi_lock()` / `half_spi_unlock()`. La logique est identique ; on centralise juste
le point de synchronisation. `half_spi_lock_init()` est appelé par `half_scan_task_start()`
avant `rf_driver_init_tx()`.

### 4.4 Skeleton-vs-stub

Entièrement réel — pas de stub. Ce module est complet à l'implémentation.

---

## 5. Brick : Trackpad (`trackpad`)

### 5.1 Hardware

Source de vérité : `~/Documents/PCB-esp/CLAUDE.md`.

| Signal | GPIO | Notes |
|---|---|---|
| SDA_TRACK | GPIO40 | I2C data, 4.7kΩ pull-up vers 3.3V |
| SCL_TRACK | GPIO38 | I2C clock, 4.7kΩ pull-up vers 3.3V |
| RST_TRACK | GPIO13 | Reset actif bas, pulse de reset au boot |
| RDY_TRACK | GPIO14 | Interrupt data-ready (actif bas), IRQ NEGEDGE |

Module : **TPS43-201A-S** (Azoteq IQS5xx-style capacitive). Branché via header J2 (6 broches
2.54mm), free-wired sur le half qui le porte. I2C ~400 kHz.

### 5.2 API (`trackpad.h`)

```c
#pragma once
#include <stdbool.h>

/* Initialize trackpad hardware:
 *  - Configure I2C master on SDA=GPIO40, SCL=GPIO38 at ~400 kHz
 *  - Pulse RST=GPIO13 low for 10 ms then high (IQS5xx reset sequence)
 *  - Configure RDY=GPIO14 as input with IRQ on NEGEDGE
 *  - Probe: attempt I2C ACK on the IQS5xx device address
 * Returns true if trackpad is physically present and responsive.
 * Returns false if no ACK (trackpad not mounted on this half). */
bool trackpad_init(void);

/* Start the trackpad task (only call if trackpad_init() returned true).
 * Creates the FreeRTOS task that waits on the RDY semaphore and sends
 * PKT_TRACKPAD packets over NRF24. */
void trackpad_start(void);
```

### 5.3 Flow (skeleton)

```
boot:
  trackpad_init()
    i2c_master_init(SDA40, SCL38, 400kHz)
    gpio_set_direction(RST13, OUTPUT)
    gpio_set_level(RST13, 0) → vTaskDelay(10ms) → gpio_set_level(RST13, 1) → vTaskDelay(50ms)
    gpio_set_direction(RDY14, INPUT)
    gpio_set_intr_type(RDY14, NEGEDGE) → isr_handler → xSemaphoreGiveFromISR(s_rdy_sem)
    i2c_probe(IQS5XX_ADDR) → returns bool (present if ACK)

runtime (trackpad_task):
  loop:
    xSemaphoreTake(s_rdy_sem, portMAX_DELAY)   ← woken by RDY14 ISR
    /* TODO STUB: read IQS5xx touch report over I2C.
     *   - Read ATI registers / data registers per IQS5xx Application Note
     *   - Extract dx, dy, finger count, button state, scroll deltas
     *   Suggested: read starting at register 0x0000 (InfoFlags), then XY data */
    rf_trackpad_t tp = { .dx = ..., .dy = ..., .buttons = ...,
                         .scroll_v = ..., .scroll_h = ..., .seq = s_seq++ };
    uint8_t buf[7];
    rf_encode_trackpad(buf, &tp);              ← already exists in rf_packet.h
    half_spi_lock();
    rf_driver_send(&s_radio, buf, 7);          ← PKT_TRACKPAD (type 0x3, already defined)
    half_spi_unlock();
```

### 5.4 Skeleton-vs-stub boundary

| Partie | Statut | Détail |
|---|---|---|
| I2C master init (GPIO40/38, 400kHz) | Skeleton réel | Code complet à l'implémentation |
| RST pulse GPIO13 | Skeleton réel | 10ms low, 50ms settle |
| RDY GPIO14 ISR + semaphore | Skeleton réel | NEGEDGE, `xSemaphoreGiveFromISR` |
| I2C probe (ACK check) | Skeleton réel | Détermine `present` |
| Lecture registres IQS5xx | **STUB** | Protocole IQS5xx, à remplir |
| Extraction dx/dy/buttons/scroll | **STUB** | Dépend de la lecture registres |
| rf_encode_trackpad + rf_driver_send | Skeleton réel | Fonctions existantes |
| half_spi_lock/unlock autour du send | Skeleton réel | Intégration verrou |

### 5.5 Intégration dongle (côté réception)

Dans `rf_rx_task.c`, le cas `PKT_TYPE_TRACKPAD` existe déjà en commentaire :

```c
/* PKT_TYPE_TRACKPAD handled in Plan 3 (mouse HID) */
```

Le brick ajoute le handler avec un hook stub :

```c
} else if (type == PKT_TYPE_TRACKPAD) {
    rf_trackpad_t tp;
    if (rf_decode_trackpad(buf, n, &tp)) {
        /* TODO STUB: forward to mouse HID.
         *   hid_send_mouse(tp.dx, tp.dy, tp.buttons, tp.scroll_v, tp.scroll_h)
         *   USB composite mouse Report ID 2 (see dongle spec Section 5).
         *   Plan 3: requires TinyUSB HID descriptor update + mouse report builder.
         *   For now, drop the packet silently. */
        (void)tp;
    }
}
```

Ce hook est une fonction faible (`__attribute__((weak))`) ou un pointeur de fonction dans
`hid_report.h` — à décider à l'implémentation. L'essentiel est que le dongle reçoive et
parse les PKT_TRACKPAD sans crasher, même si la souris n'est pas encore implémentée.

---

## 6. Brick : E-ink (`eink`)

### 6.1 Hardware

Source de vérité : `~/Documents/PCB-esp/CLAUDE.md`.

| Signal | GPIO | Notes |
|---|---|---|
| CS_DISP | GPIO18 | SPI chip select (actif bas) |
| DC_DISP | GPIO12 | Data/Command (H=data, L=command) |
| RES_DISP | GPIO17 | Reset (actif bas), pulse au boot |
| BUSY | GPIO1 | Busy (H=occupé), attendre avant nouvelle commande |

Module : **WeAct 1.54" SSD1681**, 200×200 pixels, 1bpp (1 bit par pixel), partial refresh
possible mais non prévu dans le brick (full refresh uniquement).

Partage le bus SPI2 (MOSI=GPIO48, MISO=GPIO47, SCK=GPIO45) avec le NRF24. Le SSD1681
est ajouté comme **second device SPI** sur `SPI2_HOST`, CS=GPIO18. Les 100Ω série sur le
bus sont identiques à ceux du NRF (protection seulement, pas d'isolation logique).

### 6.2 Framebuffer

200×200 px, 1bpp, MSB first : **5000 bytes** (200×200/8).

```c
#define EINK_WIDTH   200
#define EINK_HEIGHT  200
#define EINK_FB_SIZE (EINK_WIDTH * EINK_HEIGHT / 8)   /* 5000 bytes */
```

Allocation : buffer statique dans `eink.c` (pas de malloc). 5 KB stack ou BSS —
acceptable sur ESP32-S3 (520 KB SRAM).

### 6.3 API (`eink.h`)

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Initialize e-ink hardware:
 *  - Add SSD1681 as a second spi_device on SPI2_HOST (CS=GPIO18)
 *  - Configure GPIO12 (DC), GPIO17 (RES), GPIO1 (BUSY)
 *  - Pulse RES=GPIO17 low for 10 ms then high
 *  - Wait for BUSY=GPIO1 to go low (panel not busy)
 *  - Probe: check BUSY behaves correctly (heuristic: BUSY low within 200ms)
 * Returns true if panel is physically present and probe passes.
 * Returns false if BUSY stays high (panel not mounted on this half). */
bool eink_init(void);

/* Clear the panel to white (all bits = 1 in SSD1681 convention).
 * Blocks until the refresh BUSY cycle completes (~1-2 s). */
void eink_clear(void);

/* Push a full 5000-byte 1bpp framebuffer to the panel and trigger refresh.
 * fb must point to a buffer of exactly EINK_FB_SIZE bytes (5000).
 * Acquires half_spi_lock for the SPI transfer only (not for BUSY wait).
 * Returns after initiating refresh; the refresh itself runs asynchronously
 * (BUSY goes high). Caller should not push another frame until BUSY is low. */
void eink_push(const uint8_t *fb);

/* Start the e-ink refresh task (only call if eink_init() returned true).
 * The task reads half_state, renders to a local framebuffer, calls eink_push()
 * at a low rate (configurable, default ~1 Hz or on change). */
void eink_start(void);
```

### 6.4 Flow (skeleton)

```
boot:
  eink_init()
    spi_bus_add_device(SPI2_HOST, {.clock_speed_hz=4MHz, .spics_io_num=GPIO18, .mode=0}, &s_eink_dev)
    gpio_set_direction(GPIO12, OUTPUT)    /* DC */
    gpio_set_direction(GPIO17, OUTPUT)    /* RES */
    gpio_set_direction(GPIO1,  INPUT)     /* BUSY */
    /* Reset pulse */
    gpio_set_level(GPIO17, 1) → vTaskDelay(10ms) → gpio_set_level(GPIO17, 0)
    vTaskDelay(10ms)
    gpio_set_level(GPIO17, 1) → vTaskDelay(10ms)
    /* Probe: BUSY should go low after reset */
    probe: wait gpio_get_level(GPIO1) == 0, timeout 200ms → present = true/false

eink_push(const uint8_t *fb):
  /* Acquire SPI bus for the write only */
  half_spi_lock()
  /* TODO STUB: SSD1681 init + write RAM + trigger refresh.
   *   Typical sequence:
   *   1. SW reset command (0x12), wait BUSY
   *   2. Driver output control (0x01), gate/source setup
   *   3. Border waveform control (0x3C)
   *   4. Set RAM X/Y address start/end (0x44, 0x45)
   *   5. Set RAM X/Y address counter (0x4E, 0x4F)
   *   6. Write RAM B&W (0x24) + fb data (5000 bytes)
   *   7. Display update control 2 (0x22) → 0xF7 (full update sequence)
   *   8. Master activation (0x20), wait BUSY */
  half_spi_unlock()   /* unlock BEFORE waiting BUSY — panel busy, bus free */
  /* BUSY wait happens outside the lock */
  /* while (gpio_get_level(GPIO1) == 1) vTaskDelay(10ms); */

eink_start():
  xTaskCreate(eink_task, "eink", 4096, NULL, 3, NULL)

eink_task():
  loop:
    vTaskDelay(pdMS_TO_TICKS(1000))   /* 1 Hz refresh rate (e-ink content changes slowly) */
    /* TODO STUB: render half_state into local framebuffer.
     *   - Read half_state.layer_idx, half_state.layer_name[16]
     *   - Read half_state.modifiers, half_state.flags
     *   - Draw to s_fb[EINK_FB_SIZE] (1bpp, direct or via minimal font)
     *   Decision: LVGL or direct 1bpp render — see Section 7.3 */
    eink_push(s_fb)
```

### 6.5 Verrou SPI : point critique

Le BUSY wait de l'e-ink (~1-2 s pour un full refresh SSD1681) se produit **hors du verrou
SPI**. La séquence est :

1. Prendre `half_spi_lock()`.
2. Écrire le framebuffer + envoyer la commande activation (transactions SPI brèves : ~10 ms).
3. Libérer `half_spi_unlock()`.
4. Attendre `BUSY=GPIO1 bas` (jusqu'à ~2 s, panel occupe son LUT/refresh interne).

Pendant l'étape 4, le bus SPI est **libre**. Le NRF24 peut transmettre des PKT_KEY et
PKT_HEARTBEAT normalement. La frappe clavier n'est **pas bloquée** pendant le refresh e-ink.

Ce découplage est la propriété la plus importante du design partagé NRF+e-ink. Le documenter
explicitement dans les commentaires `eink.c`.

### 6.6 Skeleton-vs-stub boundary

| Partie | Statut | Détail |
|---|---|---|
| spi_bus_add_device SSD1681 | Skeleton réel | Paramètres SPI, CS GPIO18 |
| GPIO DC/RES/BUSY config | Skeleton réel | direction, niveaux |
| Reset pulse GPIO17 | Skeleton réel | 10ms low, settle |
| Probe BUSY heuristique | Skeleton réel | Timeout 200ms |
| half_spi_lock/unlock autour du transfer | Skeleton réel | Hors BUSY wait |
| Séquences de commandes SSD1681 | **STUB** | Init, write RAM, trigger refresh |
| Rendu half_state → framebuffer 1bpp | **STUB** | Texte layer, icônes |
| Approche rendu (LVGL vs direct) | **Décision ouverte** | Voir Section 7.3 |
| eink_task fréquence | Skeleton réel (1Hz) | Ajustable à l'implémentation |

### 6.7 Relation avec `display_backend.h` (existant)

`display_backend.h` définit un vtable pour les displays keyboard (OLED/round) utilisé par
`status_display.c` + LVGL port. L'e-ink half **n'utilise pas** ce vtable — le half n'a pas
`status_display.c` ni LVGL compilé (`KASE_HAS_DISPLAY=n` sur HALF). L'e-ink a sa propre API
légère (`eink.h`). Mentionné ici pour éviter la confusion à l'implémentation.

---

## 7. Brick : ESP-NOW info channel

### 7.1 Contexte et périmètre

Le design dongle (Section 8 de `2026-05-11-dongle-firmware-design.md`) décrit un ESP-NOW
**cold path on-demand** pour OTA + config push (Plan 5). Ces messages (`EN_OTA_*`,
`EN_CFG_*`) restent réservés et non implémentés ici.

Ce brick ajoute un **info channel** léger et permanent (toujours actif) pour :
- Batterie : half → dongle (périodique, ~30s)
- Layer/état : dongle → half (sur changement de layer)

Les deux usages sont suffisamment légers pour ne pas interférer avec le trafic NRF.

### 7.2 `espnow_link` — init et transport

#### API (`espnow_link.h`)

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialize ESP-NOW link:
 *  - Start WiFi in STA mode (no connection, just enables radio)
 *  - Set WiFi channel to wifi_ch from NVS namespace "rf" (default: 11)
 *  - esp_now_init()
 *  - Register recv callback → dispatches to espnow_info.c handlers
 *  - Add known peers (MACs from NVS rf.mac_left / rf.mac_right)
 *    If MACs are zero (unconfigured), log warning; skip peer add.
 * Returns true on success. */
bool espnow_link_init(void);

/* Send an ESP-NOW message to a peer.
 *  mac     : 6-byte MAC of the target peer
 *  type    : message type ID (from espnow_msg.h)
 *  payload : message payload (may be NULL if len == 0)
 *  len     : payload length in bytes
 * Returns true if esp_now_send() succeeded (fire-and-forget; no ACK tracked). */
bool espnow_send(const uint8_t mac[6], uint8_t type, const void *payload, uint16_t len);
```

#### Init WiFi + ESP-NOW (`espnow_link.c`, skeleton)

```c
bool espnow_link_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    /* Set channel from NVS rf.wifi_ch (default 6 = 2437 MHz, away from NRF channels) */
    uint8_t wifi_ch = 6;  /* TODO: load from NVS rf.wifi_ch */
    esp_wifi_set_channel(wifi_ch, WIFI_SECOND_CHAN_NONE);
    esp_now_init();
    esp_now_register_recv_cb(espnow_recv_cb);    /* dispatches to espnow_info.c */
    /* Add peers from NVS */
    /* TODO STUB: load mac_left / mac_right from NVS namespace "rf" */
    /* esp_now_peer_info_t peer = { .channel = wifi_ch, .ifidx = WIFI_IF_STA, .encrypt = false }; */
    /* memcpy(peer.peer_addr, mac_left, 6); esp_now_add_peer(&peer); */
    /* memcpy(peer.peer_addr, mac_right, 6); esp_now_add_peer(&peer); */
    ESP_LOGI(TAG, "ESP-NOW link init OK (WiFi ch %u)", wifi_ch);
    return true;
}
```

Le canal WiFi (NVS `rf.wifi_ch`, default **6** = 2437 MHz) est distinct des canaux NRF (2476/2482 MHz).
L'espacement au ch 6 est de ~39 MHz par rapport au NRF left — bien supérieur à la largeur de bande
WiFi 20 MHz. (Le dongle spec Section 8 utilisait ch 11 = 2462 MHz comme exemple ; on le remplace
par ch 6 pour maximiser la séparation avec le NRF left à 2476 MHz. Voir Section 8.1 coexistence.)

#### Peers

Les MACs sont stockées en NVS namespace `"rf"`, clés `mac_left` et `mac_right` (défini dans
la Section 6 du dongle spec). En l'absence de pairing runtime (Plan 4, hors scope), les
MACs sont **zero par défaut** : le stub log un warning et skippe l'ajout de peer. Les
handlers espnow_info.c ne peuvent pas envoyer vers les halves tant que les MACs ne sont
pas configurées.

Pour le développement/test : hardcoder les MACs dans `sdkconfig.defaults.half` via un
Kconfig de type `string` (stub — à décider à l'implémentation).

### 7.3 `espnow_msg.h` — types et structs

```c
#pragma once
#include <stdint.h>

/* ── Info-channel message type IDs ── */
/* Range 0x00-0x0F reserved for info-channel (this spec). */
#define EN_INFO_BATTERY   0x01   /* half → dongle: battery level */
#define EN_INFO_LAYER     0x02   /* dongle → half: current layer */
#define EN_INFO_STATE     0x03   /* dongle → half: modifier/flag state */

/* Range 0x10-0x1F reserved for OTA (Plan 5, dongle spec Section 8). */
/* EN_OTA_BEGIN = 0x10, EN_OTA_CHUNK = 0x11, EN_OTA_END = 0x12, EN_OTA_ACK = 0x13 */

/* Range 0x20-0x2F reserved for config push (Plan 5, dongle spec Section 8). */
/* EN_CFG_PUSH = 0x20, EN_CFG_ACK = 0x21 */

/* Range 0x30-0x3F reserved for verbose telemetry (Plan 5, dongle spec Section 8). */
/* EN_TELEM_REQ = 0x30, EN_TELEM_RSP = 0x31 */

/* ── Payload structs ── */

/* EN_INFO_BATTERY (half → dongle) — 3 bytes */
typedef struct __attribute__((packed)) {
    uint8_t batt_dV;     /* 0..83 = 0..8.3V, resolution 0.1V; 0 = unknown/stub */
    uint8_t soc_pct;     /* 0..100 = 0..100% SoC; 0 = unknown/stub */
    uint8_t charging;    /* 0 = not charging, 1 = charging (from BMS GPIO46) */
} en_battery_t;

/* EN_INFO_LAYER (dongle → half) — 17 bytes */
typedef struct __attribute__((packed)) {
    uint8_t layer_idx;   /* 0..N-1 current active layer */
    char    name[16];    /* layer name string, zero-padded */
} en_layer_t;

/* EN_INFO_STATE (dongle → half) — 2 bytes */
typedef struct __attribute__((packed)) {
    uint8_t modifiers;   /* HID modifier byte (same as report byte 0) */
    uint8_t flags;       /* bit0=caps_word, bit1=bt_connected, bit2=usb_active, bits3-7=rsvd */
} en_state_t;

/* Wire format: all messages prepend a 1-byte type ID before the payload.
 * Total ESP-NOW payload = 1 + sizeof(struct).
 * espnow_send() handles the type byte prepend internally. */
```

### 7.4 `espnow_info.c` — handlers (stubs)

```c
/* espnow_info.c — info-channel handler stubs.
 * Registered as the esp_now_recv_cb via espnow_link.c dispatch.
 * Handlers are called from the ESP-NOW receive task (not ISR). */

/* ── Dongle: receives EN_INFO_BATTERY from a half ── */
void espnow_on_battery(const uint8_t *mac, const en_battery_t *b) {
    ESP_LOGI(TAG, "battery from " MACSTR ": %u.%uV soc=%u%% chg=%u",
             MAC2STR(mac), b->batt_dV / 10, b->batt_dV % 10, b->soc_pct, b->charging);
    /* TODO STUB: forward battery level to controller via CDC.
     *   Suggested: new CDC KR push frame (unsolicited) with batt info.
     *   Or: store in a global and return via KS_CMD_DONGLE_STATS / KS_CMD_RF_LINK_STATUS. */
}

/* ── Half: receives EN_INFO_LAYER from dongle ── */
void espnow_on_layer(const en_layer_t *l) {
    /* TODO STUB: update half_state and wake e-ink task.
     *   half_state_lock();
     *   s_half_state.layer_idx = l->layer_idx;
     *   memcpy(s_half_state.layer_name, l->name, 16);
     *   half_state_unlock();
     *   xTaskNotify(s_eink_task_handle, EINK_NOTIFY_REFRESH, eSetBits); */
}

/* ── Half: receives EN_INFO_STATE from dongle ── */
void espnow_on_state(const en_state_t *s) {
    /* TODO STUB: update half_state modifiers/flags and wake e-ink task. */
}
```

#### Hook dongle `layer_changed()`

Dans `dongle_engine_state.c`, `layer_changed()` est actuellement un no-op :

```c
void layer_changed(void) { }
```

Le brick le remplace par un hook qui pousse `EN_INFO_LAYER` + `EN_INFO_STATE` :

```c
/* dongle_engine_state.c */
void layer_changed(void) {
#if CONFIG_KASE_HAS_ESPNOW
    /* TODO STUB: push EN_INFO_LAYER + EN_INFO_STATE to halves.
     *   en_layer_t l = { .layer_idx = current_layout };
     *   strncpy(l.name, get_layer_name(current_layout), 16);
     *   espnow_send(mac_left,  EN_INFO_LAYER, &l, sizeof(l));
     *   espnow_send(mac_right, EN_INFO_LAYER, &l, sizeof(l));
     *   en_state_t s = { .modifiers = ... , .flags = ... };
     *   espnow_send(mac_left,  EN_INFO_STATE, &s, sizeof(s));
     *   espnow_send(mac_right, EN_INFO_STATE, &s, sizeof(s)); */
#endif
}
```

#### Battery TX (half)

Le half envoie périodiquement `EN_INFO_BATTERY`. Candidat naturel : dans le heartbeat
timer (100 ms) avec un diviseur de fréquence (~30 s) ou une tâche séparée.

```c
/* Dans half_scan_task.c heartbeat_timer_cb ou tâche dédiée */
static uint32_t s_batt_ticks = 0;
if (++s_batt_ticks >= 300) {   /* 300 × 100ms = 30s */
    s_batt_ticks = 0;
    en_battery_t b = {
        .batt_dV  = 0,    /* TODO STUB: read ADC GPIO15 */
        .soc_pct  = 0,    /* TODO STUB: derive from batt_dV */
        .charging = 0,    /* TODO STUB: gpio_get_level(GPIO46) */
    };
    /* Send to dongle MAC (loaded from NVS rf.mac_dongle, or bcast) */
    /* espnow_send(mac_dongle, EN_INFO_BATTERY, &b, sizeof(b)); */
}
```

La MAC dongle n'est pas dans le NVS actuel — il faut l'ajouter (clé `mac_dongle` dans le
namespace `"rf"`). Stub pour l'instant : log uniquement.

### 7.5 `half_state` — struct partagée

La struct partagée entre les handlers `espnow_info.c` (écriture) et `eink.c` (lecture) :

```c
/* espnow_info.h ou half_state.h — partagé entre espnow_info.c et eink.c */
#include "freertos/semphr.h"

typedef struct {
    uint8_t layer_idx;
    char    layer_name[16];
    uint8_t modifiers;
    uint8_t flags;   /* bit0=caps_word, bit1=bt_connected, bit2=usb_active */
} half_state_t;

extern half_state_t g_half_state;
extern SemaphoreHandle_t g_half_state_mutex;

static inline void half_state_lock(void)   { xSemaphoreTake(g_half_state_mutex, portMAX_DELAY); }
static inline void half_state_unlock(void) { xSemaphoreGive(g_half_state_mutex); }
```

Initialisation dans `half_scan_task.c` (ou `espnow_info.c`). La lecture par `eink.c` copie
l'état sous mutex puis relâche avant d'appeler `eink_push()` (opération longue).

---

## 8. Préoccupations partagées

### 8.1 Coexistence 2.4 GHz : ESP-NOW (WiFi interne) ↔ NRF24

**Problème** : le half utilise simultanément la radio WiFi interne (ESP-NOW) et le module
NRF24L01+ externe (SPI). Ces deux radios opèrent sur 2.4 GHz.

**Fréquences** :
- WiFi ch 11 : 2462 MHz (centre), largeur ~20 MHz → [2452-2472 MHz]
- NRF24 left : 2476 MHz (canal 0x4C) — déborde du WiFi ch 11
- NRF24 right : 2482 MHz (canal 0x52) — clairement hors WiFi ch 11

À 2476 MHz, le NRF24 left est dans le bord du WiFi ch 11. Il existe un risque de brouillage
mutuel. Stratégies (par ordre de complexité croissante) :

| Stratégie | Description | Décision |
|---|---|---|
| **A. Tolérance** | Taux d'envoi ESP-NOW très faible (batterie ~30s, layer/état sur changement). NRF perd quelques paquets → heartbeat réconcilie. | **Défaut pour le brick** |
| **B. Canal WiFi distinct** | Utiliser WiFi ch 1 (2412 MHz) ou ch 6 (2437 MHz) — loin des NRF. Ajuster `rf.wifi_ch`. | Possible via NVS sans recompiler |
| **C. Time-windowing** | Suspendre NRF le temps de l'envoi ESP-NOW (CE=low). | Inutile pour info-channel (messages rares) |

**Décision actuelle** : stratégie A + NVS `rf.wifi_ch` par défaut à **6** (2437 MHz, centre
ch 6 = [2427-2447 MHz], 29 MHz d'écart avec NRF left à 2476 MHz). À valider au bench.

**Note** : la stratégie C (time-windowing + suspend NRF) est celle du dongle spec Section 8
pour l'OTA ESP-NOW (Plan 5). Le dongle l'applique car l'OTA est un flux soutenu longue durée.
L'info-channel est des messages ponctuels — la stratégie A suffit.

**Action bench requise** : mesurer `rf.link_q` (MAX_RT count dans PKT_HEARTBEAT) avant/après
activation ESP-NOW. Si `link_q` augmente significativement, passer au canal WiFi 1 ou 6.

### 8.2 `half_spi_lock` — portée et limites

Le verrou `half_spi_lock()` protège uniquement les **transactions SPI** (appels à
`spi_device_polling_transmit`). Il ne protège pas :
- Les GPIO CE/DC/RES (chacun n'appartient qu'à un seul driver)
- Le BUSY wait du SSD1681 (hardware interne panel, bus libre)
- Les transactions I2C du trackpad (bus différent, I2C ≠ SPI)

Conséquence : le trackpad I2C peut lire ses registres **en même temps** qu'un envoi NRF ou
e-ink sur SPI. Pas de conflit — bus séparés.

### 8.3 `half_state` — accès concurrent

`g_half_state` est écrit par le contexte ESP-NOW (task interne `esp_now`) et lu par
`eink_task`. Le mutex `g_half_state_mutex` est obligatoire. La section critique est brève
(copy struct) ; pas de risque d'inversion de priorité avec `portMAX_DELAY`.

### 8.4 Delta sdkconfig pour le half avec WiFi

`sdkconfig.defaults.half` (existant) contient `CONFIG_ESP_WIFI_ENABLED=n`. Avec ESP-NOW
actif, il faut changer :

```ini
# WiFi activé pour ESP-NOW (info-channel)
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_ESP_WIFI_NVS_ENABLED=n        # pas de persistence WiFi credentials
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n     # STA only, pas de AP
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n # pas d'enterprise auth
CONFIG_LWIP_ENABLE=n                 # ou à minima DHCP/TCP off — ESP-NOW n'a pas besoin de LwIP
```

**Impact mémoire** : WiFi ajoute ~60-80 KB de heap utilisé. ESP32-S3 a 512 KB SRAM +
PSRAM optionnel. Le half MVP sans WiFi utilisait ~80 KB heap (FreeRTOS + stacks). Avec WiFi :
~150-160 KB. Confortable — à vérifier avec `heap_caps_get_free_size()` au bench.

**Impact consommation** : WiFi en mode STA sans connexion en veille active (modem sleep non
activé) ajoute ~10-20 mA. Pour une batterie 550 mAh/cell × 2 (2S), c'est non négligeable.
`CONFIG_ESP_WIFI_MODEM_SLEEP_DEFAULT=y` + `CONFIG_PM_ENABLE=y` doivent être considérés
conjointement avec le brick battery ADC (hors scope ici).

---

## 9. Points de design ouverts (non résolus dans ces bricks)

### 9.1 Coexistence WiFi/NRF — validation bench requise

La stratégie A (tolérance, WiFi ch 6) est le défaut. Si le bench montre une dégradation
NRF significative, passer à ch 1 (2412 MHz) ou implémenter le time-windowing pour l'ESP-NOW
info-channel aussi. Pas de code prévu maintenant.

### 9.2 sdkconfig WiFi delta — heap et consommation

Le delta sdkconfig WiFi (+60-80 KB heap, +10-20 mA) doit être mesuré au bench avant de
valider l'approche. Si la RAM est trop contrainte avec e-ink framebuffer (5 KB) + stacks
trackpad/eink tâches, envisager PSRAM ou réduction des stacks.

### 9.3 Rendu e-ink — LVGL ou direct 1bpp

Deux approches :
- **Direct 1bpp** : une police de caractères intégrée (tableau de bitmaps), rendu manuel
  dans `s_fb`. Léger, sans dépendance LVGL, 0 overhead. Adapté si le contenu est simple
  (layer name, 2-3 icônes d'état).
- **LVGL** : réutilise l'écosystème existant. Nécessite de compiler LVGL + un display driver
  custom pour le 1bpp e-ink. Plus lourd (~150 KB flash) mais plus flexible pour l'UI.

**Décision déférée** : trancher à l'implémentation du stub rendu. Le brick fournit le
`eink_push(fb)` dans les deux cas — le rendu est interne à `eink_task`.

### 9.4 Source batterie `EN_INFO_BATTERY`

`batt_dV` et `soc_pct` sont 0 (unknown) dans le brick. La brick battery ADC (GPIO15 +
GPIO16 switchable ground) est un brick séparé, hors scope de ce document. Quand elle
existe, elle alimente `en_battery_t` via un getter `battery_get_dV()`.

### 9.5 MAC dongle dans NVS

La clé NVS `rf.mac_dongle` n'existe pas dans le layout NVS actuel (Section 6 du dongle
spec). Le half doit connaître la MAC du dongle pour envoyer `EN_INFO_BATTERY`. Options :
- Ajouter `mac_dongle` au namespace `"rf"` et au layout NVS.
- Broadcast ESP-NOW (MAC `FF:FF:FF:FF:FF:FF`) — tous les peers reçoivent, simple mais moins sécurisé.
- Hardcoder en Kconfig string pour le développement.

Décision déférée à l'implémentation. Stub actuel : log uniquement, pas d'envoi.

---

## 10. Hors scope de ces bricks

Les items suivants ne sont **ni spécifiés ni implémentés** dans ces bricks :

| Item | Raison |
|---|---|
| Logique complète IQS5xx (registres, protocole) | Propriétaire ; l'utilisateur implémente |
| Séquences de commandes SSD1681 | Référence datasheet/WeAct sample ; stub |
| Rendu e-ink (polices, layout) | Décision LVGL/direct déférée |
| Battery ADC brick (GPIO15/16) | Brick séparé, Phase 2+ |
| Pairing runtime (changement MAC/canal) | Plan 4, non défini |
| OTA via ESP-NOW (EN_OTA_*) | Plan 5, réservé dans espnow_msg.h |
| Config push via ESP-NOW (EN_CFG_*) | Plan 5, réservé |
| Mouse HID composite sur dongle | Plan 3 (TinyUSB descriptor, hid_report.c) |
| ESP-NOW ACK / retry applicatif | Info-channel fire-and-forget suffit |
| Light-sleep / deep-sleep (power mgmt) | Phase 2+, conjoint battery brick |

---

## Annexe A — Fichiers à créer

```
main/periph/half_spi.h
main/periph/half_spi.c

main/periph/trackpad/trackpad.h
main/periph/trackpad/trackpad.c

main/periph/eink/eink.h
main/periph/eink/eink.c

main/comm/espnow/espnow_link.h
main/comm/espnow/espnow_link.c
main/comm/espnow/espnow_msg.h
main/comm/espnow/espnow_info.c
```

## Annexe B — Fichiers à modifier

```
main/Kconfig.projbuild
    + KASE_HAS_TRACKPAD (default y if HALF)
    + KASE_HAS_EINK     (default y if HALF)
    ~ KASE_HAS_ESPNOW   (default y if DONGLE || HALF)

main/CMakeLists.txt
    if(CONFIG_KASE_HAS_EINK)
        list(APPEND srcs "periph/half_spi.c" "periph/eink/eink.c")
    endif()
    if(CONFIG_KASE_HAS_TRACKPAD)
        list(APPEND srcs "periph/trackpad/trackpad.c")
    endif()
    if(CONFIG_KASE_HAS_ESPNOW)
        list(APPEND srcs "comm/espnow/espnow_link.c" "comm/espnow/espnow_info.c")
    endif()

main/comm/rf/half_scan_task.c
    ~ s_tx_mutex → half_spi_lock() / half_spi_unlock() (refactor interne)
    + appels trackpad_init() / trackpad_start() conditionnels
    + appels eink_init() / eink_start() conditionnels
    + appel espnow_link_init() conditionnel
    + battery TX timer dans heartbeat_timer_cb

main/comm/rf/rf_rx_task.c  (dongle)
    + PKT_TYPE_TRACKPAD case → rf_decode_trackpad → hid_send_mouse stub

main/comm/rf/dongle_engine_state.c  (dongle)
    ~ layer_changed() → espnow push EN_INFO_LAYER + EN_INFO_STATE (stub)

sdkconfig.defaults.half
    ~ CONFIG_ESP_WIFI_ENABLED=n → y
    + CONFIG_ESP_WIFI_NVS_ENABLED=n
    + CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n
    + CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n
```

## Annexe C — Constantes matérielles de référence

Extrait de `~/Documents/PCB-esp/CLAUDE.md` (source de vérité GPIO) :

| Signal | GPIO | Module |
|---|---|---|
| SDA_TRACK | GPIO40 | Trackpad IQS5xx |
| SCL_TRACK | GPIO38 | Trackpad IQS5xx |
| RST_TRACK | GPIO13 | Trackpad IQS5xx |
| RDY_TRACK | GPIO14 | Trackpad IQS5xx |
| CS_DISP | GPIO18 | E-ink SSD1681 |
| DC_DISP | GPIO12 | E-ink SSD1681 |
| RES_DISP | GPIO17 | E-ink SSD1681 |
| BUSY | GPIO1 | E-ink SSD1681 |
| MOSI | GPIO48 | SPI2 partagé NRF+e-ink |
| MISO | GPIO47 | SPI2 partagé NRF+e-ink |
| SCK | GPIO45 | SPI2 partagé NRF+e-ink |
| NRF CSN | GPIO35 | NRF24L01+ |
| GPIO15 | GPIO15 | Battery ADC (ADC2_CH4) — hors scope |
| GPIO16 | GPIO16 | Battery divider GND switchable — hors scope |
| GPIO46 | GPIO46 | BMS status input — hors scope |

---

**Fin du design.**
