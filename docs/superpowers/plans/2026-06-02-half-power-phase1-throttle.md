# Half Power Management — Phase 1 (state machine + heartbeat throttle) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduire la machine à états d'énergie des moitiés (pure, testée host) et un premier effecteur **zéro-risque** : throttle dynamique du heartbeat NRF (100 ms en ACTIVE → plus lent quand inactif), sans toucher au scan ni au WiFi.

**Architecture:** Module dédié `half_power` (logique de transition pure + helper de divisor heartbeat). `half_scan_task` enregistre l'activité clavier (`s_last_activity_ms`) dans `keyboard_btn_cb`, et le callback du heartbeat timer (100 ms) calcule l'état courant et n'émet le heartbeat qu'un tick sur N (N = divisor de l'état). Le scan matrice et la TX des touches (événementielle) restent **inchangés** → latence de frappe intacte.

**Tech Stack:** C (ESP-IDF), esp_timer (heartbeat 100 ms existant), CMake host tests (`test/`).

**Référence spec:** `docs/superpowers/specs/2026-06-02-half-power-management-design.md` (Phase 1 du phasage).

**Portée Phase 1 (et ce qui est explicitement HORS Phase 1) :**
- DANS : module `half_power` pur + tests host ; suivi d'activité ; throttle du heartbeat par gate de ticks.
- HORS (→ Phase 2) : light-sleep, wake-on-GPIO, `esp_wifi_stop/start`, NRF power-down, arrêt du scan. `keyboard_button` n'ayant pas de setter de rythme à chaud, le throttle du **scan** n'est PAS fait ici (il deviendra "scan stoppé" en Phase 2 via le sleep). Gain Phase 1 = réduction des TX heartbeat NRF quand inactif (modeste) + fondation testable pour la Phase 2.

---

## File Structure

- Create: `main/comm/rf/half_power.h` — enum d'état + `half_power_next()` + `half_power_hb_divisor()` + seuils. Une responsabilité : décider l'état d'énergie (pur, sans hardware).
- Create: `main/comm/rf/half_power.c` — impl des deux fonctions pures (compilable host, aucune dépendance ESP-IDF).
- Create: `test/test_half_power.c` — tests host des deux fonctions (toutes les bornes).
- Modify: `test/CMakeLists.txt` — ajouter `test_half_power.c` + `../main/comm/rf/half_power.c` aux sources.
- Modify: `test/test_main.c` — déclarer + appeler `test_half_power()`.
- Modify: `main/comm/rf/half_scan_task.c` — `s_last_activity_ms` mis à jour dans `keyboard_btn_cb` (ligne 183) ; gate de divisor dans `heartbeat_timer_cb` (ligne 307).

---

## Task 1: Module `half_power` (pur) + tests host (TDD)

**Files:**
- Create: `main/comm/rf/half_power.h`
- Create: `main/comm/rf/half_power.c`
- Create: `test/test_half_power.c`
- Modify: `test/CMakeLists.txt`
- Modify: `test/test_main.c`

- [ ] **Step 1: Écrire le header `main/comm/rf/half_power.h`**

```c
#ifndef HALF_POWER_H
#define HALF_POWER_H

#include <stdint.h>

/* Power state of a keyboard half, by recent keyboard activity. */
typedef enum {
    HALF_POWER_ACTIVE = 0,   /* recently typed: full rate */
    HALF_POWER_THROTTLE,     /* idle a few seconds: reduced rate */
    HALF_POWER_SLEEP,        /* idle long: deep rate (real light-sleep in Phase 2) */
} half_power_state_t;

/* Idle thresholds in milliseconds (tunable). */
#define HALF_POWER_T_THROTTLE_MS  3000u
#define HALF_POWER_T_SLEEP_MS     30000u

/* Pure: decide the power state from elapsed idle time.
 * idle = now_ms - last_activity_ms. Caller passes monotonic ms (now >= last).
 * idle <  T_THROTTLE  -> ACTIVE
 * idle <  T_SLEEP     -> THROTTLE
 * else                -> SLEEP */
half_power_state_t half_power_next(uint32_t last_activity_ms, uint32_t now_ms);

/* Pure: heartbeat TX divisor for a state — send the heartbeat every Nth 100 ms
 * tick. ACTIVE=1 (100 ms), THROTTLE=5 (500 ms), SLEEP=10 (1 s). */
uint8_t half_power_hb_divisor(half_power_state_t state);

#endif /* HALF_POWER_H */
```

- [ ] **Step 2: Écrire les tests host `test/test_half_power.c`**

```c
#include "test_framework.h"
#include "../main/comm/rf/half_power.h"

void test_half_power(void)
{
    printf("\n--- half_power ---\n");

    /* half_power_next: bornes de T_THROTTLE (3000) et T_SLEEP (30000) */
    TEST_ASSERT_EQ(half_power_next(0, 0),     HALF_POWER_ACTIVE,   "idle 0 -> ACTIVE");
    TEST_ASSERT_EQ(half_power_next(0, 2999),  HALF_POWER_ACTIVE,   "idle 2999 -> ACTIVE");
    TEST_ASSERT_EQ(half_power_next(0, 3000),  HALF_POWER_THROTTLE, "idle 3000 -> THROTTLE");
    TEST_ASSERT_EQ(half_power_next(0, 29999), HALF_POWER_THROTTLE, "idle 29999 -> THROTTLE");
    TEST_ASSERT_EQ(half_power_next(0, 30000), HALF_POWER_SLEEP,    "idle 30000 -> SLEEP");
    TEST_ASSERT_EQ(half_power_next(0, 99999), HALF_POWER_SLEEP,    "idle large -> SLEEP");

    /* activité récente avec base non nulle (now > last) */
    TEST_ASSERT_EQ(half_power_next(100000, 100500), HALF_POWER_ACTIVE,
                   "activité il y a 500ms -> ACTIVE");
    TEST_ASSERT_EQ(half_power_next(100000, 135000), HALF_POWER_SLEEP,
                   "activité il y a 35s -> SLEEP");

    /* half_power_hb_divisor */
    TEST_ASSERT_EQ(half_power_hb_divisor(HALF_POWER_ACTIVE),   1,  "divisor ACTIVE = 1");
    TEST_ASSERT_EQ(half_power_hb_divisor(HALF_POWER_THROTTLE), 5,  "divisor THROTTLE = 5");
    TEST_ASSERT_EQ(half_power_hb_divisor(HALF_POWER_SLEEP),    10, "divisor SLEEP = 10");
}
```

- [ ] **Step 3: Déclarer + appeler le test dans `test/test_main.c`**

Ajouter la déclaration extern avec les autres (près des autres `extern void test_*`):
```c
extern void test_half_power(void);
```
Et l'appel dans `main()` avec les autres appels `test_*();` (ex. après `test_rf_signal_q255();`):
```c
    test_half_power();
```

- [ ] **Step 4: Ajouter les sources à `test/CMakeLists.txt`**

Dans le `add_executable(test_runner ...)`, ajouter (à côté de `test_rf_signal_q255.c` / `../main/comm/rf/rf_rx_task.c`):
```cmake
    test_half_power.c
    ../main/comm/rf/half_power.c
```

- [ ] **Step 5: Lancer les tests — DOIT échouer (rouge TDD)**

Run:
```bash
rm -rf test/build && cmake -S test -B test/build >/dev/null && cmake --build test/build 2>&1 | tail -15
```
Expected: échec de **link/compilation** — `half_power_next` / `half_power_hb_divisor` indéfinis (half_power.c pas encore écrit). C'est l'état rouge attendu.

- [ ] **Step 6: Écrire l'implémentation `main/comm/rf/half_power.c`**

```c
#include "half_power.h"

half_power_state_t half_power_next(uint32_t last_activity_ms, uint32_t now_ms)
{
    uint32_t idle = now_ms - last_activity_ms;   /* monotonic ms; wrap ~49j accepté */
    if (idle < HALF_POWER_T_THROTTLE_MS) return HALF_POWER_ACTIVE;
    if (idle < HALF_POWER_T_SLEEP_MS)    return HALF_POWER_THROTTLE;
    return HALF_POWER_SLEEP;
}

uint8_t half_power_hb_divisor(half_power_state_t state)
{
    switch (state) {
        case HALF_POWER_ACTIVE:   return 1;
        case HALF_POWER_THROTTLE: return 5;
        case HALF_POWER_SLEEP:    return 10;
        default:                  return 1;
    }
}
```

- [ ] **Step 7: Relancer les tests — DOIT passer (vert)**

Run:
```bash
cmake --build test/build >/dev/null 2>&1 && ./test/build/test_runner | tail -4
```
Expected: `--- half_power ---` listé et `Results: N passed, 0 failed`.

- [ ] **Step 8: Commit**

```bash
git add main/comm/rf/half_power.h main/comm/rf/half_power.c test/test_half_power.c test/CMakeLists.txt test/test_main.c
git commit -m "feat(half): half_power state machine (pure) + host tests"
```
Note: le hook `pre-push` ne se déclenche qu'au push ; un commit normal passe. Si un hook bloque, `git commit --no-verify`.

---

## Task 2: Intégrer le throttle heartbeat dans `half_scan_task.c`

**Files:**
- Modify: `main/comm/rf/half_scan_task.c` (include, `s_last_activity_ms`, `keyboard_btn_cb` @183, `heartbeat_timer_cb` @307)

**Contexte exact :**
- `keyboard_btn_cb` (ligne 183) est appelé à **tout changement de matrice** → point d'enregistrement de l'activité.
- `heartbeat_timer_cb` (ligne 307) tourne toutes les **100 ms** ; il fait la retry + construit/émet `PKT_HEARTBEAT`. On y ajoute un gate : n'émettre que si `tick % divisor == 0`.
- `esp_timer_get_time() / 1000` = millisecondes (déjà utilisé ligne 213).

- [ ] **Step 1: Inclure le module + déclarer l'activité**

En haut de `half_scan_task.c`, après les autres `#include "..."` du dossier rf (ex. après `#include "rf_pairing.h"`), ajouter :
```c
#include "half_power.h"   /* half_power_next / half_power_hb_divisor */
```
Près des autres statics de fichier (ex. après `static uint8_t s_pressed_bitmap[RF_HALF_BITMAP_BYTES];`, ligne 71), ajouter :
```c
/* Last keyboard activity (ms, esp_timer). Drives the power state machine. */
static volatile uint32_t s_last_activity_ms = 0;
```

- [ ] **Step 2: Marquer l'activité dans `keyboard_btn_cb`**

Dans `keyboard_btn_cb` (ligne 183), au tout début du corps (après les `(void)` casts), ajouter :
```c
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
```
Résultat attendu du bloc :
```c
static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle,
                            keyboard_btn_report_t  kbd_report,
                            void                  *user_data)
{
    (void)kbd_handle;
    (void)user_data;

    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* Delegate to half_diff_emit: ... */
    half_diff_emit(
        s_pressed_bitmap,
        kbd_report.key_data,         kbd_report.key_pressed_num,
        kbd_report.key_release_data, kbd_report.key_release_num,
        tx_key_event, NULL);
}
```

- [ ] **Step 3: Gate du heartbeat selon l'état dans `heartbeat_timer_cb`**

Dans `heartbeat_timer_cb` (ligne 307), juste après `(void)arg;`, calculer l'état + le divisor et décider si on émet le heartbeat ce tick. Le compteur de tick heartbeat existe déjà sous une autre forme (`s_stat_ticks` pour le print 10 s) — on ajoute un compteur dédié pour le gate.

Ajouter au début du callback (après `(void)arg;`):
```c
    /* Power state -> heartbeat throttle. The 100 ms timer keeps firing (cheap);
     * we only TX the heartbeat every Nth tick when idle, cutting NRF TX. Key
     * events (event-driven) are unaffected → typing latency unchanged. */
    static uint32_t s_hb_tick = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    half_power_state_t pstate = half_power_next(s_last_activity_ms, now_ms);
    uint8_t hb_div = half_power_hb_divisor(pstate);
    bool emit_hb = (s_hb_tick % hb_div) == 0;
    s_hb_tick++;
```
Puis englober la **construction + TX du heartbeat** (le bloc `rf_heartbeat_t hb; ... rf_driver_send(&s_radio, buf, 9); ...` jusqu'à la fin du `if (!ok) {...}`) dans `if (emit_hb) { ... }`. La **retry** (bloc `if (s_has_pending_retry) {...}` au-dessus) et le **print 10 s** (`s_stat_ticks`) restent exécutés à chaque tick (inchangés).

Concrètement, le heartbeat TX (lignes ~323-349) devient :
```c
    if (emit_hb) {
        /* Build and transmit PKT_HEARTBEAT */
        rf_heartbeat_t hb;
        memset(&hb, 0, sizeof(hb));
        memcpy(hb.bitmap, s_pressed_bitmap, RF_HALF_BITMAP_BYTES);
        hb.batt_dV = 0;   /* MVP: battery not measured */
        {
            uint32_t txc = rf_tx_count, rsum = rf_tx_retr_sum;
            hb.link_q = (txc > 0) ? (uint8_t)((rsum * 100u) / (txc * 3u)) : 0;
            rf_tx_retr_sum = 0;
            rf_tx_count    = 0;
        }
        s_stat_last_lq = hb.link_q;
        hb.seq     = s_seq++;
        s_stat_maxrt += rf_tx_max_rt_count;
        rf_tx_max_rt_count = 0;

        uint8_t buf[9];
        rf_encode_heartbeat(buf, &hb);
        half_spi_lock();
        bool ok = rf_driver_send(&s_radio, buf, 9);
        half_spi_unlock();
        if (!ok) {
            ESP_LOGD(TAG, "heartbeat TX failed (MAX_RT)");
        }
        /* 10 s console status accounting (only on emitted heartbeats) */
        s_stat_hb_total++;
        if (ok) s_stat_hb_ok++;
    }
```
Note: laisser le bloc `s_stat_ticks`/`half_status_print()` (toutes les 100 ticks) HORS du `if (emit_hb)` pour que le status console s'imprime toujours toutes les 10 s. Adapter si le code original comptait `s_stat_hb_total` ailleurs — ne le compter que sur heartbeat émis (comme ci-dessus).

- [ ] **Step 4: Initialiser `s_last_activity_ms` au boot (état ACTIVE au démarrage)**

Là où le heartbeat timer est démarré (ligne ~532, `esp_timer_start_periodic(hb_timer, 100 * 1000)`), juste avant, ajouter :
```c
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
```
(sinon `s_last_activity_ms=0` au boot ferait croire à une longue inactivité → SLEEP immédiat).

- [ ] **Step 5: Build des 2 boards moitiés**

Run:
```bash
source ~/esp/esp-idf/export.sh
for b in kase_half_left kase_half_right; do
  idf.py -B build_$b -DBOARD=$b -DSDKCONFIG=build_$b/sdkconfig build >/tmp/b_$b.log 2>&1 \
    && echo "✓ $b" || { echo "✗ $b"; tail -15 /tmp/b_$b.log; }
done
```
Expected: `✓ kase_half_left` et `✓ kase_half_right`.

- [ ] **Step 6: Vérifier que les tests host restent verts**

Run: `./scripts/check.sh --host-only 2>&1 | tail -2`
Expected: `✓ check.sh: tout vert`.

- [ ] **Step 7: Commit**

```bash
git add main/comm/rf/half_scan_task.c
git commit -m "feat(half): dynamic heartbeat throttle driven by half_power state"
```

---

## Task 3: Documenter le smoke-test Phase 1

**Files:**
- Modify: `docs/HARDWARE_SMOKE_TEST.md` (section Half)

- [ ] **Step 1: Ajouter les cases Phase 1 sous la section Half**

Ajouter dans la section `## Half (left / right)` de `docs/HARDWARE_SMOKE_TEST.md` :
```markdown
- [ ] Power Phase 1 : frappe reste instantanée (scan/TX inchangés)
- [ ] Power Phase 1 : après ~3 s sans frappe, le heartbeat ralentit (console half : cadence du `half_stat` / TX heartbeat espacée) sans perte de lien
- [ ] Power Phase 1 : reprise immédiate du heartbeat 100 ms à la première frappe
```

- [ ] **Step 2: Commit**

```bash
git add docs/HARDWARE_SMOKE_TEST.md
git commit -m "docs: smoke-test Half power Phase 1 (heartbeat throttle)"
```

---

## Self-Review

**Spec coverage (Phase 1 du phasage) :**
- Machine à états pure + testée host → Task 1 ✓ (half_power_next, toutes bornes).
- Effecteur heartbeat dynamique (ACTIVE/THROTTLE/SLEEP divisors) → Task 1 (divisor) + Task 2 (gate) ✓.
- Suivi d'activité → Task 2 (keyboard_btn_cb) ✓.
- Scan/latence inchangés → garanti (on ne touche que le gate heartbeat) ✓.
- Hors Phase 1 (sleep/WiFi/scan-stop) explicitement reporté Phase 2 ✓.
- Smoke-test → Task 3 ✓.

**Placeholders :** aucun (code complet à chaque étape).

**Cohérence des types :** `half_power_state_t`, `half_power_next(last, now)`, `half_power_hb_divisor(state)` identiques entre header (T1.1), tests (T1.2), impl (T1.6), intégration (T2.3). Seuils `HALF_POWER_T_THROTTLE_MS`/`_T_SLEEP_MS` cohérents.

## Notes
- Gain Phase 1 = réduction des TX heartbeat NRF quand inactif + fondation. Le gros gain batterie (WiFi off, scan stoppé, light-sleep) est en **Phase 2** (plan séparé).
- Phase 2 réutilisera `half_power_next()` (état SLEEP) pour déclencher le light-sleep réel + wake-on-GPIO.
