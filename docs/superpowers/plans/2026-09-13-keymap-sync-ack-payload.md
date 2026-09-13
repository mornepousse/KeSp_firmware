# Sync auto keymap dongle→gauche (ACK payload) — Plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Synchroniser automatiquement la keymap du dongle vers la moitié gauche par radio, sans jamais brancher la gauche, en glissant les données dans les ACK payloads ESB de ses émissions normales.

**Architecture:** La gauche parle déjà au dongle en sans-fil (STATUS + MATRIX, auto-ACK matériel). On active les ACK payloads nRF24 : le dongle glisse la keymap (par chunks) dans l'ACK des trames de la gauche. Pull piloté par la gauche (elle demande le prochain chunk manquant dans un paquet uplink ; le dongle sert dans l'ACK). L'empreinte CRC-32 du garde-fou existant (`b1db4091`) sert d'accusé de bout en bout : le drip s'arrête dès `match=1`.

**Tech Stack:** C, ESP-IDF 5.5, nRF24L01+ (ESB, DPL, ACK payload), tests host CMake.

**Spec:** `docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md`

## Global Constraints

- Tout est derrière `CONFIG_KASE_DONGLE_FUSION` (défaut off). Les 7 boards par défaut restent verts.
- `KEYMAP_BLOB_BYTES` = 1120 o = 10 couches × 4 rangées × 14 colonnes × 2 (sur niphar_left/dongle-fusion, `KEYMAP_COLS`=14).
- Chunk = **28 o** → exactement **40 chunks** (1120 = 40 × 28, aucun partiel).
- ACK payload ≤ 32 o. Types de paquet libres : `0x7`=SYNC_BEACON, `0x8`=SYNC_CHUNK, `0x9`=SYNC_REQ (0x1–0x6 pris, 0xE/0xF = pairing).
- Source de vérité = le dongle (USB-C gauche power-only). Push dongle→gauche inconditionnel sur divergence, pas de versioning (YAGNI).
- v1 = **keymap seule**. Macros/combos/etc. hors périmètre.
- Zone RF « une puce, un propriétaire » : chaque puce a un seul propriétaire (dongle=rf_rx_task, gauche=kbd_relay). Vider la FIFO AVANT d'émettre, jamais après.
- Norme TDD pour la logique pure ; toute source touchée doit être adossée à un `[test:X]` ou une ligne de `COMPORTEMENTS.md`.

---

### Task 1: Trames de sync (encode/decode, logique pure)

**Files:**
- Modify: `main/comm/rf/rf_packet.h`
- Test: `test/test_keymap_sync_frames.c` (create), `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces:**
- Produces:
  - `#define PKT_TYPE_SYNC_BEACON 0x7`, `PKT_TYPE_SYNC_CHUNK 0x8`, `PKT_TYPE_SYNC_REQ 0x9`
  - `#define SYNC_CHUNK_BYTES 28`, `#define SYNC_N_CHUNKS 40`
  - `typedef struct { uint32_t fp_target; uint8_t n_chunks; } rf_sync_beacon_t;`
  - `typedef struct { uint8_t idx; uint8_t data[SYNC_CHUNK_BYTES]; } rf_sync_chunk_t;`
  - `typedef struct { uint8_t next; } rf_sync_req_t;`
  - `uint16_t rf_encode_sync_beacon(uint8_t *buf, const rf_sync_beacon_t *b);` → 6 o : `[type<<4][fp LE 4][n_chunks]`
  - `bool rf_decode_sync_beacon(const uint8_t *buf, uint16_t len, rf_sync_beacon_t *o);`
  - `uint16_t rf_encode_sync_chunk(uint8_t *buf, const rf_sync_chunk_t *c);` → 30 o : `[type<<4][idx][data 28]`
  - `bool rf_decode_sync_chunk(const uint8_t *buf, uint16_t len, rf_sync_chunk_t *o);`
  - `uint16_t rf_encode_sync_req(uint8_t *buf, const rf_sync_req_t *q);` → 2 o : `[type<<4][next]`
  - `bool rf_decode_sync_req(const uint8_t *buf, uint16_t len, rf_sync_req_t *o);`

- [x] **Step 1: Write the failing test**

`test/test_keymap_sync_frames.c` :
```c
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"
#include <string.h>

static void test_beacon_roundtrip(void)
{
    rf_sync_beacon_t in = { .fp_target = 0xDEADBEEFu, .n_chunks = SYNC_N_CHUNKS };
    uint8_t buf[8];
    uint16_t n = rf_encode_sync_beacon(buf, &in);
    TEST_ASSERT_EQ(n, 6, "beacon = 6 octets");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_SYNC_BEACON, "type BEACON");
    rf_sync_beacon_t out = {0};
    TEST_ASSERT(rf_decode_sync_beacon(buf, n, &out), "decode beacon");
    TEST_ASSERT_EQ(out.fp_target, 0xDEADBEEFu, "fp_target round-trip");
    TEST_ASSERT_EQ(out.n_chunks, SYNC_N_CHUNKS, "n_chunks round-trip");
    /* mauvais type rejeté */
    uint8_t bad[6]; memcpy(bad, buf, 6); bad[0] = (PKT_TYPE_STATUS << 4);
    TEST_ASSERT(!rf_decode_sync_beacon(bad, 6, &out), "rejette autre type");
    TEST_ASSERT(!rf_decode_sync_beacon(buf, 5, &out), "rejette trop court");
}

static void test_chunk_roundtrip(void)
{
    rf_sync_chunk_t in = { .idx = 39 };
    for (int i = 0; i < SYNC_CHUNK_BYTES; i++) in.data[i] = (uint8_t)(i * 3 + 1);
    uint8_t buf[32];
    uint16_t n = rf_encode_sync_chunk(buf, &in);
    TEST_ASSERT_EQ(n, 30, "chunk = 30 octets");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_SYNC_CHUNK, "type CHUNK");
    rf_sync_chunk_t out = {0};
    TEST_ASSERT(rf_decode_sync_chunk(buf, n, &out), "decode chunk");
    TEST_ASSERT_EQ(out.idx, 39, "idx round-trip");
    TEST_ASSERT(memcmp(out.data, in.data, SYNC_CHUNK_BYTES) == 0, "data round-trip");
    TEST_ASSERT(!rf_decode_sync_chunk(buf, 29, &out), "rejette trop court");
}

static void test_req_roundtrip(void)
{
    rf_sync_req_t in = { .next = 7 };
    uint8_t buf[4];
    uint16_t n = rf_encode_sync_req(buf, &in);
    TEST_ASSERT_EQ(n, 2, "req = 2 octets");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_SYNC_REQ, "type REQ");
    rf_sync_req_t out = {0};
    TEST_ASSERT(rf_decode_sync_req(buf, n, &out), "decode req");
    TEST_ASSERT_EQ(out.next, 7, "next round-trip");
}

void test_keymap_sync_frames(void)
{
    TEST_SUITE("Trames de sync keymap");
    test_beacon_roundtrip();
    test_chunk_roundtrip();
    test_req_roundtrip();
}
```
Ajouter `test_keymap_sync_frames.c` à `test/CMakeLists.txt` (près de `test_rf_packet.c`), et dans `test/test_main.c` : `extern void test_keymap_sync_frames(void);` + `test_keymap_sync_frames();`.

- [x] **Step 2: Run test to verify it fails**

Run: `cd test && cmake --build build -j && ./build/test_runner 2>&1 | grep -iE "sync|FAIL|Results"`
Expected: FAIL (fonctions/constantes non définies) ou erreur de link.

- [x] **Step 3: Write minimal implementation**

Dans `rf_packet.h`, près des autres `PKT_TYPE_*` et encoders :
```c
#define PKT_TYPE_SYNC_BEACON 0x7
#define PKT_TYPE_SYNC_CHUNK  0x8
#define PKT_TYPE_SYNC_REQ    0x9
#define SYNC_CHUNK_BYTES     28
#define SYNC_N_CHUNKS        40   /* 1120 / 28 */

typedef struct { uint32_t fp_target; uint8_t n_chunks; } rf_sync_beacon_t;
typedef struct { uint8_t idx; uint8_t data[SYNC_CHUNK_BYTES]; } rf_sync_chunk_t;
typedef struct { uint8_t next; } rf_sync_req_t;

static inline uint16_t rf_encode_sync_beacon(uint8_t *buf, const rf_sync_beacon_t *b)
{
    if (!buf || !b) return 0;
    buf[0] = (PKT_TYPE_SYNC_BEACON << 4);
    buf[1] = (uint8_t)(b->fp_target);       buf[2] = (uint8_t)(b->fp_target >> 8);
    buf[3] = (uint8_t)(b->fp_target >> 16); buf[4] = (uint8_t)(b->fp_target >> 24);
    buf[5] = b->n_chunks;
    return 6;
}
static inline bool rf_decode_sync_beacon(const uint8_t *buf, uint16_t len, rf_sync_beacon_t *o)
{
    if (!buf || !o || len < 6 || rf_packet_type(buf, len) != PKT_TYPE_SYNC_BEACON) return false;
    o->fp_target = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) |
                   ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
    o->n_chunks = buf[5];
    return true;
}
static inline uint16_t rf_encode_sync_chunk(uint8_t *buf, const rf_sync_chunk_t *c)
{
    if (!buf || !c) return 0;
    buf[0] = (PKT_TYPE_SYNC_CHUNK << 4);
    buf[1] = c->idx;
    memcpy(&buf[2], c->data, SYNC_CHUNK_BYTES);
    return 2 + SYNC_CHUNK_BYTES;   /* 30 */
}
static inline bool rf_decode_sync_chunk(const uint8_t *buf, uint16_t len, rf_sync_chunk_t *o)
{
    if (!buf || !o || len < 2 + SYNC_CHUNK_BYTES ||
        rf_packet_type(buf, len) != PKT_TYPE_SYNC_CHUNK) return false;
    o->idx = buf[1];
    memcpy(o->data, &buf[2], SYNC_CHUNK_BYTES);
    return true;
}
static inline uint16_t rf_encode_sync_req(uint8_t *buf, const rf_sync_req_t *q)
{
    if (!buf || !q) return 0;
    buf[0] = (PKT_TYPE_SYNC_REQ << 4);
    buf[1] = q->next;
    return 2;
}
static inline bool rf_decode_sync_req(const uint8_t *buf, uint16_t len, rf_sync_req_t *o)
{
    if (!buf || !o || len < 2 || rf_packet_type(buf, len) != PKT_TYPE_SYNC_REQ) return false;
    o->next = buf[1];
    return true;
}
```
(`rf_packet.h` inclut déjà `<string.h>` pour `memcpy` — vérifier ; sinon l'ajouter.)

- [x] **Step 4: Run test to verify it passes**

Run: `cd test && cmake --build build -j && ./build/test_runner 2>&1 | grep -iE "Results"`
Expected: PASS, compteur augmenté.

- [x] **Step 5: Commit**

```bash
git add main/comm/rf/rf_packet.h test/test_keymap_sync_frames.c test/CMakeLists.txt test/test_main.c .tripwire-testcount
git commit -m "feat(keymap-sync): trames BEACON/CHUNK/REQ (encode/decode, testé)"
```

---

### Task 2: Réassembleur de keymap (logique pure, pilote le pull)

**Files:**
- Create: `main/comm/rf/keymap_sync.h`
- Test: `test/test_keymap_sync.c` (create), `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces:**
- Consumes: `SYNC_CHUNK_BYTES`, `SYNC_N_CHUNKS` (Task 1).
- Produces:
  - `typedef struct { uint8_t buf[SYNC_N_CHUNKS * SYNC_CHUNK_BYTES]; uint8_t next; } keymap_rx_t;`
  - `void keymap_rx_reset(keymap_rx_t *s);`
  - `bool keymap_rx_chunk(keymap_rx_t *s, uint8_t idx, const uint8_t *data);` — applique le chunk SI `idx == s->next` ; renvoie true s'il a avancé (chunk accepté), false s'il l'a ignoré (doublon / hors séquence).
  - `uint8_t keymap_rx_next(const keymap_rx_t *s);` — prochain chunk voulu (0..SYNC_N_CHUNKS).
  - `bool keymap_rx_complete(const keymap_rx_t *s);` — true quand `next == SYNC_N_CHUNKS`.

- [x] **Step 1: Write the failing test**

`test/test_keymap_sync.c` :
```c
/* Réassemblage séquentiel piloté par le pull : la gauche ne demande QUE le
 * prochain chunk manquant, le dongle le sert. Donc l'ordre est garanti ; le
 * réassembleur n'accepte que le chunk attendu et ignore doublons / hors-séquence
 * (un ACK payload rejoué ou une trame en retard ne corrompt rien). */
#include "test_framework.h"
#include "../main/comm/rf/keymap_sync.h"
#include <string.h>

static void remplir(uint8_t *d, uint8_t seed)
{ for (int i = 0; i < SYNC_CHUNK_BYTES; i++) d[i] = (uint8_t)(seed + i); }

static void test_sequence_complete(void)
{
    keymap_rx_t s; keymap_rx_reset(&s);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 0, "démarre au chunk 0");
    TEST_ASSERT(!keymap_rx_complete(&s), "pas complet au départ");
    for (int k = 0; k < SYNC_N_CHUNKS; k++) {
        uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, (uint8_t)k);
        TEST_ASSERT(keymap_rx_chunk(&s, (uint8_t)k, d), "chunk en séquence accepté");
        TEST_ASSERT_EQ(keymap_rx_next(&s), (uint8_t)(k + 1), "next avance");
    }
    TEST_ASSERT(keymap_rx_complete(&s), "complet après 40 chunks");
    /* le blob réassemblé est exact */
    for (int k = 0; k < SYNC_N_CHUNKS; k++) {
        uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, (uint8_t)k);
        TEST_ASSERT(memcmp(&s.buf[k * SYNC_CHUNK_BYTES], d, SYNC_CHUNK_BYTES) == 0,
                    "contenu du chunk à la bonne place");
    }
}

static void test_doublon_et_hors_sequence_ignores(void)
{
    keymap_rx_t s; keymap_rx_reset(&s);
    uint8_t d0[SYNC_CHUNK_BYTES]; remplir(d0, 0);
    TEST_ASSERT(keymap_rx_chunk(&s, 0, d0), "chunk 0 accepté");
    /* doublon de 0 : ignoré, next inchangé */
    TEST_ASSERT(!keymap_rx_chunk(&s, 0, d0), "doublon ignoré");
    TEST_ASSERT_EQ(keymap_rx_next(&s), 1, "next reste 1 après doublon");
    /* saut à 5 (hors séquence) : ignoré */
    uint8_t d5[SYNC_CHUNK_BYTES]; remplir(d5, 5);
    TEST_ASSERT(!keymap_rx_chunk(&s, 5, d5), "hors séquence ignoré");
    TEST_ASSERT_EQ(keymap_rx_next(&s), 1, "next reste 1 après hors-séquence");
}

void test_keymap_sync(void)
{
    TEST_SUITE("Réassembleur keymap (pull séquentiel)");
    test_sequence_complete();
    test_doublon_et_hors_sequence_ignores();
}
```
Enregistrer dans `test/CMakeLists.txt` + `test/test_main.c`.

- [x] **Step 2: Run test to verify it fails**

Run: `cd test && cmake --build build -j && ./build/test_runner 2>&1 | grep -iE "Réassembleur|FAIL|Results"`
Expected: FAIL / erreur de compilation (header absent).

- [x] **Step 3: Write minimal implementation**

`main/comm/rf/keymap_sync.h` :
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rf_packet.h"   /* SYNC_CHUNK_BYTES, SYNC_N_CHUNKS */

/* Réassemblage de la keymap reçue par chunks (drip via ACK payload). Séquentiel :
 * la gauche demande toujours le prochain chunk manquant, donc on n'accepte que
 * `next` et on ignore doublons / hors-séquence — un ACK rejoué ne corrompt rien.
 * Pur, testé host (test/test_keymap_sync.c). */
typedef struct {
    uint8_t buf[SYNC_N_CHUNKS * SYNC_CHUNK_BYTES];   /* 1120 o */
    uint8_t next;                                    /* prochain chunk attendu */
} keymap_rx_t;

static inline void keymap_rx_reset(keymap_rx_t *s) { s->next = 0; }

static inline bool keymap_rx_chunk(keymap_rx_t *s, uint8_t idx, const uint8_t *data)
{
    if (idx != s->next || s->next >= SYNC_N_CHUNKS) return false;
    memcpy(&s->buf[(size_t)idx * SYNC_CHUNK_BYTES], data, SYNC_CHUNK_BYTES);
    s->next++;
    return true;
}

static inline uint8_t keymap_rx_next(const keymap_rx_t *s) { return s->next; }
static inline bool keymap_rx_complete(const keymap_rx_t *s) { return s->next >= SYNC_N_CHUNKS; }
```

- [x] **Step 4: Run test to verify it passes** — puis prouver qu'il mord : muter `idx != s->next` en `false` (accepte tout), rebuild, vérifier des FAIL, revenir (⚠ `keymap_sync.h` est neuf/non commité — sauvegarder en scratchpad avant mutation, PAS `git checkout`).

Run: `cd test && cmake --build build -j && ./build/test_runner 2>&1 | grep Results`
Expected: PASS.

- [x] **Step 5: Commit**

```bash
git add main/comm/rf/keymap_sync.h test/test_keymap_sync.c test/CMakeLists.txt test/test_main.c .tripwire-testcount
git commit -m "feat(keymap-sync): réassembleur séquentiel (pur, testé, mordant)"
```

---

### Task 3: Primitives ACK payload dans le driver (RF, prouvé au banc)

**Files:**
- Modify: `main/comm/rf/rf_driver.c`, `main/comm/rf/rf_driver.h`

**Interfaces:**
- Produces:
  - `void rf_driver_load_ack_payload(rf_radio_t *r, uint8_t pipe, const uint8_t *data, uint8_t len);` — PRX : charge la charge utile qui partira dans le prochain ACK (`W_ACK_PAYLOAD` = `0xA8 | pipe`).
  - `bool rf_driver_send_ap(rf_radio_t *r, const uint8_t *buf, uint8_t len, uint8_t *ack_out, uint8_t *ack_len);` — PTX : émet, et si l'ACK portait une charge utile (RX_DR levé), la copie dans `ack_out`/`ack_len` (≤32). Renvoie l'ACK (TX_DS) comme `rf_driver_send`. `ack_len=0` si pas de payload.

**Note RF (pas de test host — vérification au banc) :** activer `EN_ACK_PAY` impose `FEATURE = 0x06` (EN_DPL|EN_ACK_PAY), avec EN_AA et DPL déjà présents. Le PTX détecte l'ACK-avec-charge par `RX_DR` (STATUS bit6, 0x40) après `TX_DS`, puis lit par `R_RX_PAYLOAD` (0x61) et vide (`FLUSH_RX` si besoin).

- [x] **Step 1:** Passer les trois écritures `REG_FEATURE, 0x04` à `0x06` dans `rf_driver.c` (init PRX ligne ~353, ré-armements ~398/430, et la config partagée ~593). Ajouter en tête les commandes manquantes :
```c
#ifndef CMD_W_ACK_PAYLOAD
#define CMD_W_ACK_PAYLOAD(pipe) (0xA8 | ((pipe) & 0x07))
#endif
```

- [x] **Step 2:** Implémenter `rf_driver_load_ack_payload` (modèle : la séquence CSN + `spi_xfer` de `rf_driver_send`, mais commande `CMD_W_ACK_PAYLOAD(pipe)` suivie des `len` octets ; aucun pulse CE). Sous le mutex du propriétaire (l'appelant le tient).

- [x] **Step 3:** Ajouter `rf_driver_send_ap` : corps identique à `rf_driver_send` (flush TX AVANT, W_TX_PAYLOAD, pulse CE, poll TX_DS/MAX_RT), puis APRÈS `TX_DS` : si `status & 0x40` (RX_DR), lire la largeur (`R_RX_PL_WID` 0x60), `R_RX_PAYLOAD` dans `ack_out`, poser `*ack_len`, effacer RX_DR (`REG_STATUS`, 0x40). Sinon `*ack_len = 0`. `rf_driver_send` devient `rf_driver_send_ap(r,buf,len,NULL,NULL)`.

- [x] **Step 4 (banc, étape 1 du spec) :** build dongle+fusion et niphar_left+fusion, flasher (dongle CH340 `ttyUSB0`, gauche FTDI `ttyUSB2`, app-only `0x20000`, MAC vérifié). Test « hello » : câbler temporairement le dongle pour charger un ACK payload connu (ex. 4 octets `0xA5A5A5A5`) sur réception d'une trame du slot clavier ; côté gauche, loguer `ack_len`/`ack_out` après émission RF. **Critère : la gauche logue les octets connus.** Si les clones nRF24 ne suivent pas (RX_DR jamais levé) → consigner, et déclencher le repli (approche B) noté au spec.

- [x] **Step 5: Commit**

```bash
git add main/comm/rf/rf_driver.c main/comm/rf/rf_driver.h
git commit -m "feat(rf): ACK payload nRF24 (EN_ACK_PAY, load/send_ap) — canal retour PRX→PTX"
```

---

### Task 4: Le dongle sert balise + chunks dans l'ACK (RF, banc)

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c`, `main/comm/rf/dongle_engine.c`, `main/comm/rf/dongle_engine.h`

**Interfaces:**
- Consumes: `rf_encode_sync_beacon/chunk` (Task 1), `rf_driver_load_ack_payload` (Task 3), `config_fp_crc32`, `dongle_engine_get_coherence` (garde-fou existant).
- Produces (dongle_engine) :
  - `bool dongle_sync_active(void);` — true tant qu'une divergence est connue (`match=0` et `left_fp != 0`).
  - `uint16_t dongle_sync_ack_for(uint8_t req_next, uint8_t *out);` — construit la charge d'ACK à charger : si `req_next < SYNC_N_CHUNKS`, un CHUNK `req_next` (tranché dans `keymaps`), sinon une BEACON `{own_fp, SYNC_N_CHUNKS}`. Renvoie la longueur.

- [x] **Step 1:** Dans `dongle_engine.c`, ajouter `dongle_sync_active()` (à partir de `dongle_engine_get_coherence`) et `dongle_sync_ack_for()` : tranche `((const uint8_t*)keymaps)[req_next*SYNC_CHUNK_BYTES ...]` dans un `rf_sync_chunk_t` puis `rf_encode_sync_chunk`, ou `rf_encode_sync_beacon` si `req_next >= SYNC_N_CHUNKS`. Déclarer les deux dans `dongle_engine.h` (sous le `#if CONFIG_KASE_DONGLE_FUSION`).

- [x] **Step 2:** Dans `rf_rx_task.c` `drain_radio`, slot clavier : après avoir traité une trame et TANT QUE `dongle_sync_active()`, précharger l'ACK payload pour la PROCHAINE trame de la gauche. Défaut `req_next = SYNC_N_CHUNKS` (⇒ balise) ; si la trame reçue est un `PKT_TYPE_SYNC_REQ`, décoder `.next` et charger le CHUNK correspondant. `rf_driver_load_ack_payload(&s_kbd, 0, buf, len)` sous le propriétaire unique (rf_rx_task). Quand `!dongle_sync_active()` : ne rien charger (silence, coût nul).

- [x] **Step 3 (banc, étapes 2-3 du spec) :** flasher le dongle. Provoquer une divergence (garde-fou `match=0`, déjà le cas si les keymaps diffèrent). Avec la gauche en sans-fil (FTDI = alim, USB natif débranché), observer via un log dongle temporaire : à réception d'une trame gauche, l'ACK préchargé = BEACON `{own_fp, 40}` ; puis, si la gauche demande le chunk 0 (Task 5), l'ACK = CHUNK 0. **Critère : balise chargée sur mismatch ; chunk demandé servi.**

- [x] **Step 4: Commit**

```bash
git add main/comm/rf/rf_rx_task.c main/comm/rf/dongle_engine.c main/comm/rf/dongle_engine.h
git commit -m "feat(keymap-sync): le dongle sert balise + chunks dans l'ACK sur divergence"
```

---

### Task 5: La gauche tire, réassemble et enregistre (RF, banc)

**Files:**
- Modify: `main/comm/rf/kbd_relay_tx.c`

**Interfaces:**
- Consumes: `rf_driver_send_ap` (Task 3), `keymap_rx_*` (Task 2), `rf_decode_sync_beacon/chunk`, `rf_encode_sync_req` (Task 1), `keymaps`, `KEYMAP_BLOB_BYTES`, `save_keymaps`, `config_fp_crc32`.
- État interne : `static keymap_rx_t s_krx; static bool s_syncing; static uint32_t s_sync_target_fp;`

- [x] **Step 1:** Faire passer les émissions du chemin sans-fil de la gauche par `rf_driver_send_ap` (au lieu de `rf_driver_send`) pour récupérer l'ACK payload — dans `kbd_tx_locked` (relais/refresh RF). Après chaque envoi, si `ack_len > 0`, décoder :
  - `rf_decode_sync_beacon` → si `fp_target != config_fp_crc32(keymaps, KEYMAP_BLOB_BYTES)` et `!s_syncing` : `keymap_rx_reset(&s_krx)`, `s_syncing = true`, `s_sync_target_fp = fp_target`.
  - `rf_decode_sync_chunk` → `keymap_rx_chunk(&s_krx, idx, data)`.
- [x] **Step 2:** Tant que `s_syncing`, la gauche émet périodiquement un `PKT_TYPE_SYNC_REQ{ .next = keymap_rx_next(&s_krx) }` (via `kbd_tx_locked`, cadence ~ celle du STATUS) pour piloter le pull. À `keymap_rx_complete(&s_krx)` : `memcpy((uint8_t*)keymaps, s_krx.buf, KEYMAP_BLOB_BYTES)`, `save_keymaps((uint16_t*)keymaps, KEYMAP_BLOB_BYTES)`, `s_syncing = false`. Le STATUS suivant annoncera la nouvelle empreinte (le champ `config_fp` est déjà calculé à la volée) → le dongle verra `match=1` et cessera la balise.
- [x] **Step 3:** Garde-fous : borne le débit des SYNC_REQ (ne pas noyer le lien pendant la frappe) ; ne synchroniser qu'en route RF (jamais en USB — en USB la gauche a déjà la main sur sa keymap). Respecter le propriétaire unique (kbd_relay) et le mutex `s_tx_mutex`.

- [x] **Step 4 (banc, étape 4 du spec) :** flasher gauche + dongle. Divergence provoquée (éditer la keymap du dongle par CDC SETLAYER). **Sans rien brancher d'autre**, gauche en sans-fil : attendre < 1 min, puis interroger le dongle par CDC 0x17 → **`match=1`**. Vérifier aussi qu'en branchant ensuite la gauche seule en USB, elle tape bien la keymap synchronisée. **Critère : `match` repasse à 1 sans câble, et la gauche tape juste en standalone.**

- [x] **Step 5: Commit**

```bash
git add main/comm/rf/kbd_relay_tx.c
git commit -m "feat(keymap-sync): la gauche tire la keymap par ACK, réassemble et enregistre"
```

---

### Task 6: Robustesse, contrat, docs

**Files:**
- Modify: `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`, `docs/CDC_BINARY_PROTOCOL.md` (note sur 0x17 = accusé de sync), `docs/superpowers/plans/2026-09-13-dongle-fusion-runtime.md` (marquer phase 3 en cours)

- [ ] **Step 1 (banc, étape 5 du spec) :** éprouver : ACK perdus (la gauche redemande le même chunk, pas de corruption) ; sync EN PLEINE FRAPPE (pas de touche perdue/collée) ; **zéro trafic sync une fois `match=1`** (la balise cesse — vérifier à la console dongle qu'aucun ACK payload n'est plus chargé au repos synchronisé).
- [ ] **Step 2:** Ajouter à `COMPORTEMENTS.md`, section « Fusion — sync auto » :
  - `[test:test_keymap_sync]` Le réassembleur n'accepte que le prochain chunk attendu ; doublons et hors-séquence sont ignorés (un ACK rejoué ne corrompt pas la keymap).
  - `[test:test_beacon_roundtrip]` (ou le nom retenu) Les trames de sync survivent à l'encode/decode.
  - `[smoke:Sync keymap sans câble]` Une divergence dongle↔gauche se résorbe seule par RF (ACK payload), sans brancher la gauche ; `match` repasse à 1.
  Ajouter l'item smoke correspondant à `docs/HARDWARE_SMOKE_TEST.md` (section Dongle) : « Sync keymap sans câble : éditer la keymap du dongle → en <1 min sans-fil, CDC 0x17 match repasse à 1 ; la gauche en USB tape la nouvelle keymap ».
- [ ] **Step 3:** `TRIPWIRE_CONTRAT_STRICT=1 ./scripts/check.sh --fast` vert ; build dongle+fusion / niphar_left+fusion / niphar_right+fusion rc=0 ; `./scripts/check.sh` (7 boards) vert.
- [ ] **Step 4: Commit + push**

```bash
git add COMPORTEMENTS.md docs/HARDWARE_SMOKE_TEST.md docs/CDC_BINARY_PROTOCOL.md docs/superpowers/plans/2026-09-13-dongle-fusion-runtime.md
git commit -m "feat(keymap-sync): robustesse validée, contrat + docs"
git push origin main   # dans le devshell : pre-push = 7 boards
```

---

## Notes d'exécution

- **Ordre :** Tasks 1-2 (pur, host-testable, sûres) d'abord ; puis 3 (driver, la brique la plus risquée — si les clones nRF24 ne gèrent pas les ACK payloads, s'arrêter là et remonter au repli B) ; puis 4-5 (dongle/gauche) ; 6 (durcissement/docs).
- **RF = banc obligatoire.** Les Tasks 3-5 n'ont pas de test host (registres nRF24) ; leur oracle est l'observation d'ACK réels, étape par étape, exactement comme les trois pannes silencieuses de CLAUDE.md l'imposent. Ne pas enchaîner deux étapes RF sans preuve intermédiaire.
- **Piège FIFO :** vider la FIFO AVANT d'émettre (jamais après), y compris pour les ACK payloads (cf. CLAUDE.md, « un ACK ESB ne prouve pas la réception logicielle »).
- **Mutation sur fichiers neufs :** pour prouver qu'un test mord sur un `.h` non commité, sauvegarder en scratchpad et restaurer de là — jamais `git checkout` (il revient à HEAD et efface le travail non commité ; erreur commise deux fois cette session).
