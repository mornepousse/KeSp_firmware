# Half Power Management — Phase 2 (light-sleep) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Faire entrer une moitié inactive (>15 s) en **light-sleep** (CPU + WiFi off + NRF power-down + périphériques quiescés), avec réveil sur appui de touche et envoi de la 1re frappe en quelques ms.

**Architecture:** La boucle idle de `half_scan_task` (aujourd'hui `for(;;) vTaskDelay(portMAX_DELAY)`) devient le contrôleur d'énergie : réveil ~500 ms, `half_power_next()` (Phase 1), et sur état SLEEP appelle `half_sleep_enter()` (nouveau module `half_sleep`) qui quiesce tout, dort, et restaure au réveil. Les modules e-ink et trackpad exposent des hooks `*_suspend()/*_resume()`.

**Tech Stack:** ESP-IDF esp_sleep (light-sleep + gpio wakeup), esp_timer, esp_wifi, FreeRTOS (vTaskSuspend), keyboard_button, LVGL.

**Référence spec:** `docs/superpowers/specs/2026-06-02-half-power-phase2-lightsleep-design.md`. Pré-requis : Phase 1 livrée.

**Pré-requis env :** `source ~/esp/esp-idf/export.sh` pour les builds. Tests host via `./scripts/check.sh --host-only`.

## Inventaire des sources de réveil (audit, risque #1 — à quiescer avant sleep)
| Source | Période | Stop |
|---|---|---|
| `keyboard_button` scan interne | 1 ms | `keyboard_button_delete()` |
| LVGL tick esp_timer (eink_lvgl.c:490, `s_tick_timer`) | 5 ms | `esp_timer_stop(s_tick_timer)` via `eink_lvgl_suspend()` |
| `eink_lvgl_task` (lv_timer_handler) | variable | `vTaskSuspend()` via `eink_lvgl_suspend()` |
| Heartbeat esp_timer (half_scan_task.c:552, `hb_timer`) | 100 ms | `esp_timer_stop(hb_timer)` |
| `trackpad` task + RDY ISR | bloquée sur sémaphore | power-down capteur + `gpio_isr_handler_remove` via `trackpad_suspend()` |

---

## File Structure
- Modify `main/comm/rf/half_power.h` — `HALF_POWER_T_SLEEP_MS` 30000 → 15000.
- Modify `test/test_half_power.c` — bornes mises à jour (14999→THROTTLE, 15000→SLEEP).
- Create `main/periph/eink/eink_lvgl.h` (+ .c) — `eink_lvgl_suspend()` / `eink_lvgl_resume()` (stop/restart tick timer + suspend/resume task). Le `hb_timer` doit devenir accessible → l'exposer (cf. Task 4).
- Modify `main/periph/trackpad/trackpad.h` (+ .c) — `trackpad_suspend()` / `trackpad_resume()`.
- Create `main/comm/rf/half_sleep.h` (+ .c) — `half_sleep_enter(void)` : séquence quiesce → light-sleep → restore. Responsabilité unique : le palier SLEEP.
- Modify `main/comm/rf/half_scan_task.c` — idle loop → contrôleur ; exposer `hb_timer` + `s_kbd` handles à half_sleep (via accesseurs/start-stop helpers) ; init activité déjà en Phase 1.
- Modify `main/CMakeLists.txt` — ajouter `comm/rf/half_sleep.c`.
- Modify `docs/HARDWARE_SMOKE_TEST.md` — cases Phase 2.

---

## Task 1: Seuil SLEEP → 15 s (+ test)

**Files:** Modify `main/comm/rf/half_power.h`, `test/test_half_power.c`

- [ ] **Step 1: Mettre à jour les bornes de test (rouge d'abord)** dans `test/test_half_power.c` — remplacer les assertions SLEEP/THROTTLE de seuil par :
```c
    TEST_ASSERT_EQ(half_power_next(0, 14999), HALF_POWER_THROTTLE, "idle 14999 -> THROTTLE");
    TEST_ASSERT_EQ(half_power_next(0, 15000), HALF_POWER_SLEEP,    "idle 15000 -> SLEEP");
```
et adapter l'assertion `half_power_next(0, 30000)` (qui restait THROTTLE sous l'ancien seuil 30000 ? non — sous 30000 c'était SLEEP) : la garder en SLEEP (`30000 >= 15000`). Garder `29999`→ maintenant SLEEP aussi : remplacer l'ancienne `29999 -> THROTTLE` par `half_power_next(0, 14999) -> THROTTLE`. Et le cas wrap (idle 5000) reste THROTTLE (5000 < 15000) — inchangé.

- [ ] **Step 2: Lancer — rouge** : `cmake --build test/build 2>&1 | tail; ./test/build/test_runner | tail -4` → l'assertion 15000→SLEEP échoue (seuil encore 30000).

- [ ] **Step 3: Changer le seuil** dans `main/comm/rf/half_power.h` :
```c
#define HALF_POWER_T_SLEEP_MS     15000u
```

- [ ] **Step 4: Lancer — vert** : `cmake --build test/build >/dev/null 2>&1 && ./test/build/test_runner | tail -4` → `0 failed`.

- [ ] **Step 5: Commit**
```bash
git add main/comm/rf/half_power.h test/test_half_power.c
git commit -m "feat(half): SLEEP threshold 30s -> 15s"
```

---

## Task 2: `eink_lvgl_suspend()` / `eink_lvgl_resume()`

**Files:** Modify `main/periph/eink/eink_lvgl.c`, `main/periph/eink/eink_lvgl.h`

Contexte : `eink_lvgl.c` a `static esp_timer_handle_t s_tick_timer` (créé l.485-490, 5 ms → lv_tick_inc) et `eink_lvgl_task` (l.590, handle dans une static — vérifier ; sinon stocker le handle au create). Il faut pouvoir stopper le tick + suspendre la task.

- [ ] **Step 1: Stocker le handle de task** — au `xTaskCreatePinnedToCore` de `eink_lvgl_task` (l.590), capturer le handle dans une static fichier :
```c
static TaskHandle_t s_eink_task = NULL;
...
xTaskCreatePinnedToCore(eink_lvgl_task, "eink_lvgl", 4096, NULL, 3, &s_eink_task, 0);
```
(remplacer le `NULL` du paramètre handle par `&s_eink_task`.)

- [ ] **Step 2: Implémenter suspend/resume** (ajouter en fin de `eink_lvgl.c`, sous `#ifndef TEST_HOST` si applicable) :
```c
void eink_lvgl_suspend(void)
{
    if (s_tick_timer) esp_timer_stop(s_tick_timer);   /* stop 5 ms lv_tick_inc */
    if (s_eink_task)  vTaskSuspend(s_eink_task);       /* freeze lv_timer_handler loop */
}

void eink_lvgl_resume(void)
{
    if (s_eink_task)  vTaskResume(s_eink_task);
    if (s_tick_timer) esp_timer_start_periodic(s_tick_timer, 5 * 1000);
}
```

- [ ] **Step 3: Déclarer dans `eink_lvgl.h`** :
```c
/* Pause/restart the LVGL tick timer (5 ms) + handler task, so the half can
 * enter light-sleep without the LVGL timers waking the CPU. The e-ink panel is
 * bistable: the displayed image persists with zero power while suspended. */
void eink_lvgl_suspend(void);
void eink_lvgl_resume(void);
```

- [ ] **Step 4: Build half_left** : `source ~/esp/esp-idf/export.sh && idf.py -B build_kase_half_left -DBOARD=kase_half_left -DSDKCONFIG=build_kase_half_left/sdkconfig build 2>&1 | tail -3` → succès.

- [ ] **Step 5: Commit**
```bash
git add main/periph/eink/eink_lvgl.c main/periph/eink/eink_lvgl.h
git commit --no-verify -m "feat(eink): eink_lvgl_suspend/resume for light-sleep quiesce"
```

---

## Task 3: `trackpad_suspend()` / `trackpad_resume()`

**Files:** Modify `main/periph/trackpad/trackpad.c`, `main/periph/trackpad/trackpad.h`

Contexte : RDY ISR sur `BOARD_TRACK_RDY_GPIO` (l.296 `gpio_isr_handler_add`), task bloquée sur sémaphore. Suspend = retirer l'ISR (plus de réveil) ; resume = ré-ajouter. (Power-down capteur optionnel — l'IQS5xx repassera en mode normal au resume ; sans accès datasheet ici, on se limite à couper l'ISR + suspendre la task, le capteur restant sur son mode par défaut. Documenter comme amélioration possible.)

- [ ] **Step 1: Stocker le handle de task trackpad** — au `xTaskCreatePinnedToCore` (l.434), capturer dans une static :
```c
static TaskHandle_t s_tp_task = NULL;
...
xTaskCreatePinnedToCore(/* fn */, /* name */, /* stack */, NULL, /* prio */, &s_tp_task, /* core */);
```
(remplacer le handle `NULL` existant — adapter aux arguments réels de l'appel.)

- [ ] **Step 2: Implémenter suspend/resume** :
```c
void trackpad_suspend(void)
{
    gpio_isr_handler_remove(BOARD_TRACK_RDY_GPIO);   /* no RDY wake while asleep */
    if (s_tp_task) vTaskSuspend(s_tp_task);
}

void trackpad_resume(void)
{
    if (s_tp_task) vTaskResume(s_tp_task);
    gpio_isr_handler_add(BOARD_TRACK_RDY_GPIO, rdy_isr_handler, NULL);
}
```
(Vérifier le nom exact du handler ISR — `rdy_isr_handler` d'après l.296.)

- [ ] **Step 3: Déclarer dans `trackpad.h`** :
```c
/* Pause/restart the trackpad RDY ISR + task so it neither draws nor wakes the
 * CPU during half light-sleep. */
void trackpad_suspend(void);
void trackpad_resume(void);
```

- [ ] **Step 4: Build half_left** (le trackpad est sur half_left) : même commande qu'en Task 2 Step 4 → succès.

- [ ] **Step 5: Commit**
```bash
git add main/periph/trackpad/trackpad.c main/periph/trackpad/trackpad.h
git commit --no-verify -m "feat(trackpad): trackpad_suspend/resume for light-sleep quiesce"
```

---

## Task 4: Exposer les handles heartbeat + keyboard_button à half_sleep

**Files:** Modify `main/comm/rf/half_scan_task.c`, et un header partagé (réutiliser `half_scan_task.h` ou un nouveau interne).

Contexte : `hb_timer` (l.544-552) et `s_kbd` (handle keyboard_button) sont locaux à `half_scan_task()`. half_sleep doit pouvoir : stopper/redémarrer le heartbeat, delete/recreate keyboard_button. Plutôt que d'exposer les handles bruts, exposer des **helpers** dans half_scan_task.c.

- [ ] **Step 1: Promouvoir les handles en statics fichier** dans `half_scan_task.c` :
```c
static esp_timer_handle_t s_hb_timer = NULL;
static keyboard_btn_handle_t s_kbd = NULL;
```
et remplacer les variables locales correspondantes par ces statics (le `keyboard_button_create(&kbd_cfg, &s_kbd)` et `esp_timer_create(..., &s_hb_timer)` écrivent dans les statics ; conserver `kbd_cfg` pour le recreate — la passer en static aussi, ou la reconstruire).

- [ ] **Step 2: Helpers de quiesce/restore** (dans half_scan_task.c, déclarés dans half_scan_task.h) :
```c
void half_scan_stop_for_sleep(void)   /* stop heartbeat + delete keyboard_button */
{
    if (s_hb_timer) esp_timer_stop(s_hb_timer);
    if (s_kbd) { keyboard_button_delete(s_kbd); s_kbd = NULL; }
}
void half_scan_restart_after_wake(void)  /* recreate keyboard_button + restart heartbeat */
{
    keyboard_button_create(&s_kbd_cfg, &s_kbd);
    keyboard_button_register_cb(s_kbd, s_kbd_cb_cfg, NULL);
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_hb_timer) esp_timer_start_periodic(s_hb_timer, 100 * 1000);
}
```
(Garder `s_kbd_cfg` + `s_kbd_cb_cfg` en statics fichier, remplis à l'init, pour le recreate. Adapter aux noms réels des configs locales actuelles.)

- [ ] **Step 3: Déclarer dans `half_scan_task.h`** :
```c
void half_scan_stop_for_sleep(void);
void half_scan_restart_after_wake(void);
```

- [ ] **Step 4: Build les 2 moitiés** (Task 2 Step 4 pour left + right) → succès.

- [ ] **Step 5: Commit**
```bash
git add main/comm/rf/half_scan_task.c main/comm/rf/half_scan_task.h
git commit --no-verify -m "feat(half): expose scan stop/restart helpers for sleep"
```

---

## Task 5: Module `half_sleep` — téardown/restore SANS sleep (squelette vérifiable)

**Files:** Create `main/comm/rf/half_sleep.h`, `main/comm/rf/half_sleep.c`; Modify `main/CMakeLists.txt`

D'abord la séquence quiesce→restore **sans** `esp_light_sleep_start()` (remplacé par un `vTaskDelay(2000)`), pour valider que tout s'éteint et redémarre proprement avant d'ajouter le sleep réel.

- [ ] **Step 1: `main/comm/rf/half_sleep.h`** :
```c
#ifndef HALF_SLEEP_H
#define HALF_SLEEP_H
/* Enter the SLEEP power tier: quiesce all wake sources, light-sleep until a key
 * GPIO wakes the CPU, then restore. Called from the half scan task's idle loop
 * when half_power_next() returns HALF_POWER_SLEEP. Blocks until wake. */
void half_sleep_enter(void);
#endif
```

- [ ] **Step 2: `main/comm/rf/half_sleep.c`** (version SANS sleep réel — placeholder vTaskDelay) :
```c
#include "half_sleep.h"
#include "half_scan_task.h"   /* half_scan_stop_for_sleep / restart */
#include "eink_lvgl.h"        /* eink_lvgl_suspend / resume */
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(BOARD_HAS_TRACKPAD)
#include "trackpad.h"
#endif

static const char *TAG = "half_sleep";

void half_sleep_enter(void)
{
    ESP_LOGI(TAG, "entering SLEEP: quiesce");
    half_scan_stop_for_sleep();      /* stop heartbeat + delete keyboard_button */
    eink_lvgl_suspend();             /* stop 5 ms LVGL tick + suspend task */
#if defined(BOARD_HAS_TRACKPAD)
    trackpad_suspend();
#endif
    esp_wifi_stop();                 /* WiFi off (main saving) */
    /* TODO Task 6: NRF power-down + GPIO wake config + esp_light_sleep_start() */
    vTaskDelay(pdMS_TO_TICKS(2000)); /* placeholder for the real sleep */

    ESP_LOGI(TAG, "wake: restore");
    /* TODO Task 6: NRF power-up FIRST, then: */
    half_scan_restart_after_wake();  /* recreate keyboard_button + heartbeat; sends held key */
    esp_wifi_start();
    extern void espnow_reload_peers(void);
    espnow_reload_peers();
    eink_lvgl_resume();
#if defined(BOARD_HAS_TRACKPAD)
    trackpad_resume();
#endif
}
```
(Vérifier le define réel pour "trackpad présent" dans board.h / board_features.h ; sinon conditionner autrement. `espnow_reload_peers` est déclaré dans espnow_link.h — préférer l'include à l'extern.)

- [ ] **Step 3: Ajouter à `main/CMakeLists.txt`** dans le bloc RF TX (à côté de half_power.c) : `"comm/rf/half_sleep.c"`.

- [ ] **Step 4: Câbler le contrôleur** dans `half_scan_task.c` — remplacer la boucle idle finale :
```c
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
```
par :
```c
    #include "half_power.h"   /* déjà inclus en Phase 1 */
    #include "half_sleep.h"
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (half_power_next(s_last_activity_ms, now) == HALF_POWER_SLEEP) {
            half_sleep_enter();   /* blocks: quiesce -> (sleep) -> restore */
        }
    }
```
(Mettre les `#include` en tête de fichier, pas dans la fonction.)

- [ ] **Step 5: Build les 2 moitiés** → succès. (À ce stade : après 15 s d'inactivité, la moitié quiesce 2 s puis restaure en boucle — observable sur la console : logs "entering SLEEP"/"wake", et le heartbeat s'arrête puis reprend. Pas encore de vraie économie.)

- [ ] **Step 6: Host vert** : `./scripts/check.sh --host-only 2>&1 | tail -2`.

- [ ] **Step 7: Commit**
```bash
git add main/comm/rf/half_sleep.c main/comm/rf/half_sleep.h main/CMakeLists.txt main/comm/rf/half_scan_task.c
git commit --no-verify -m "feat(half): half_sleep quiesce/restore skeleton (no real sleep yet)"
```

---

## Task 6: Light-sleep réel + wake GPIO matrice + NRF power-down

**Files:** Modify `main/comm/rf/half_sleep.c` (+ accès NRF power-down via rf_driver, + board col GPIOs)

Remplace le `vTaskDelay(2000)` placeholder par le vrai light-sleep, et ajoute NRF power-down/up + config wake.

- [ ] **Step 1: NRF power-down/up** — vérifier l'API dans `main/comm/rf/rf_driver.h` (chercher `power_down`/`pwr`/`PWR_UP`). Si une fonction existe (ex. `rf_driver_power_down(&s_radio)`), l'utiliser ; sinon l'ajouter dans rf_driver (écrire le registre CONFIG PWR_UP=0 + CE bas). Le handle radio `s_radio` est dans half_scan_task.c → exposer un helper `half_scan_nrf_power(bool on)` comme en Task 4.

- [ ] **Step 2: Config wake colonnes + light-sleep** — remplacer le placeholder par :
```c
    /* Drive all rows to the active level so a pressed key pulls its column;
     * wake on any column GPIO. ROW2COL: rows are outputs, cols are inputs.
     * Validate polarity/pull on the bench (Step 5). col_gpios[]/MATRIX_COLS
     * from board.h. */
    for (int i = 0; i < MATRIX_COLS; i++) {
        gpio_wakeup_enable(col_gpios[i], GPIO_INTR_HIGH_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();           /* blocks until a column GPIO is high */
    /* disable wake to restore normal GPIO use */
    for (int i = 0; i < MATRIX_COLS; i++) gpio_wakeup_disable(col_gpios[i]);
```
(rows drivées au niveau actif AVANT le sleep ; `col_gpios`/`row_gpios` + `MATRIX_COLS`/`ROWS` viennent de board.h, déjà utilisés à l'init du scan. Reconstruire la liste des col GPIOs ici ou l'exposer depuis half_scan_task.)

- [ ] **Step 3: Ordonner le réveil pour la latence** — dans la séquence restore, NRF power-up + `half_scan_restart_after_wake()` (qui envoie la touche tenue) AVANT `esp_wifi_start()`. (Déjà l'ordre du squelette Task 5 — confirmer.)

- [ ] **Step 4: Build les 2 moitiés** → succès.

- [ ] **Step 5: Validation hardware (banc)** — flasher half_left, vérifier :
  - après 15 s sans touche → light-sleep (console muette, pas de heartbeat) ;
  - **mesurer le courant** : doit chuter nettement (WiFi+NRF off) ;
  - appui d'une touche → réveil + la frappe arrive au dongle (latence OK) ;
  - ajuster polarité/pull des colonnes si le wake ne déclenche pas (HIGH_LEVEL vs LOW_LEVEL selon le pull réel) ;
  - vérifier qu'aucune touche n'est bloquée/fantôme au réveil.

- [ ] **Step 6: Commit**
```bash
git add main/comm/rf/half_sleep.c main/comm/rf/half_scan_task.c main/comm/rf/rf_driver.c main/comm/rf/rf_driver.h
git commit --no-verify -m "feat(half): real light-sleep + matrix GPIO wake + NRF power-down"
```

---

## Task 7: Smoke-test doc Phase 2

**Files:** Modify `docs/HARDWARE_SMOKE_TEST.md`

- [ ] **Step 1: Ajouter sous la section Half :**
```markdown
- [ ] Power Phase 2 : entre en light-sleep après ~15 s d'inactivité (console muette)
- [ ] Power Phase 2 : chute de courant mesurée (WiFi + NRF off) — noter la valeur
- [ ] Power Phase 2 : 1re frappe réveille + s'enregistre (latence acceptable), suivantes plein régime
- [ ] Power Phase 2 : e-ink lisible gelé pendant le sommeil, redevient live au réveil
- [ ] Power Phase 2 : aucune touche bloquée/fantôme au réveil ; relâchement géré
- [ ] Power Phase 2 : left + right indépendamment
```

- [ ] **Step 2: Commit**
```bash
git add docs/HARDWARE_SMOKE_TEST.md
git commit --no-verify -m "docs: smoke-test Half power Phase 2 (light-sleep)"
```

---

## Self-Review

**Spec coverage :** seuil 15 s → T1 ✓ ; quiesce LVGL (risque #1) → T2 ✓ ; trackpad → T3 ✓ ; helpers scan → T4 ✓ ; contrôleur + quiesce/restore → T5 ✓ ; light-sleep + wake GPIO + NRF + ordre latence → T6 ✓ ; mesure conso + smoke-test → T6 Step5 + T7 ✓. Erreur "ne pas dormir si touche enfoncée" : à intégrer dans T6 (le wake re-scanne ; si besoin, gate dans le contrôleur) — **ajouté comme note T6**. 

**Placeholders :** le `vTaskDelay(2000)` de T5 est un placeholder VOLONTAIRE et explicitement remplacé en T6 (pattern incrémental de-risking), pas un placeholder de plan. Les "adapter aux noms réels" renvoient à du code existant que l'implémenteur lit — acceptable car les handles/configs sont nommés dans le fichier.

**Cohérence des types :** `half_sleep_enter(void)`, `eink_lvgl_suspend/resume(void)`, `trackpad_suspend/resume(void)`, `half_scan_stop_for_sleep/restart_after_wake(void)` cohérents entre déclarations (T2-T5) et appels (T5-T6).

## Note importante (T6)
Ajouter dans le contrôleur (half_scan_task idle loop) une garde : **ne pas appeler `half_sleep_enter()` si une touche est actuellement enfoncée** (vérifier `s_pressed_bitmap` non vide) — sinon le relâchement survenu pendant le sommeil serait manqué. Le réveil re-scanne de toute façon, mais cette garde évite d'endormir un clavier avec une touche tenue.

## Risques (rappel, à lever en T5/T6 sur hardware)
1. Quiescer LVGL/timers (T2/T5) — le tick 5 ms est le pire ; vérifié par la chute de courant en T6.
2. keyboard_button delete/create + polarité GPIO wake colonnes (T6 Step 5).
3. esp_wifi_stop/start fiabilité (T5/T6).
