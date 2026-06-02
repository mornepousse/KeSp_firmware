# Design — Power management dynamique des moitiés

Date : 2026-06-02
Statut : approuvé (brainstorming) → à transformer en plan d'implémentation

## Problème / objectif

Les moitiés du clavier split (boards `kase_half_left`/`kase_half_right`, batterie)
tournent en **plein régime permanent** : scan matrice 1000Hz (1 ms), WiFi allumé
en continu pour le RX ESP-NOW (status d'affichage), heartbeat NRF 100 ms, CPU
jamais en sleep. Aucun `WIFI_PS` / light-sleep / deep-sleep (`CONFIG_PM_ENABLE=n`).

**Objectif** : maximiser l'**autonomie batterie des moitiés** (la chauffe baisse
en bonus). Le **dongle est hors scope** (USB-alimenté, récepteur → conso
incompressible).

### Contraintes / décisions issues du brainstorming
- **Rythme dynamique** : plein 1000Hz quand on tape (latence ~1 ms, "gamer-grade"),
  dégradé quand inactif. Clarification importante : les touches sont déjà
  envoyées **à l'événement** (sur changement de matrice), pas au rythme du
  heartbeat — le "10Hz" historique est le heartbeat/keepalive, pas la latence.
- **Status e-ink** : live **pendant l'usage**, gelé quand inactif. L'e-ink est
  bistable → garde son image à **zéro courant**, donc un status figé reste
  lisible sans consommer.
- **3 paliers** : `ACTIVE → THROTTLE → SLEEP`, escalade par inactivité.
- **Architecture A** : module dédié `half_power` ; logique de transition **pure
  et testée host** ; effecteurs hardware derrière une interface ;
  `half_scan_task` signale l'activité.

## Architecture

```
keyboard_btn_cb (changement matrice)
        │  notifie l'activité
        ▼
half_power (module dédié)
  ├─ half_power_next(last_activity_ms, now_ms) → état   ← PUR, testé host
  └─ effecteurs (impl hardware) : scan rate / WiFi / NRF / light-sleep
        │ applique l'état
        ▼
half_scan_task / espnow_link / rf_driver (NRF) / eink
```

### Unités
- **`half_power_next()`** — fonction pure. Entrées : `last_activity_ms`,
  `now_ms`, (seuils en constantes). Sortie : `HALF_POWER_ACTIVE | THROTTLE |
  SLEEP`. Aucune dépendance hardware → testable host. Une responsabilité :
  décider l'état.
- **Effecteurs** (`half_power_apply(state)`) — applique l'état aux périphériques.
  Dépend de : keyboard_button (scan rate / stop), esp_wifi, esp_now, rf_driver
  (NRF power-down/up), esp_sleep (light-sleep + gpio wakeup). Hardware, non
  testé host.
- **`half_scan_task`** — inchangé dans sa fonction de scan/TX ; ajoute : pousser
  `last_activity_ms` à chaque event clavier, et un tick qui appelle
  `half_power_next` + `half_power_apply` sur transition.

## Machine à états

```
half_power_next(last_activity_ms, now_ms):
    idle = now_ms - last_activity_ms
    if idle < T_THROTTLE_MS   → ACTIVE     (défaut T_THROTTLE_MS = 3000)
    elif idle < T_SLEEP_MS    → THROTTLE   (défaut T_SLEEP_MS   = 30000)
    else                      → SLEEP
```
Toute activité clavier remet `last_activity_ms = now` → retour ACTIVE depuis
n'importe quel état. Seuils = `#define` configurables.

### Effecteurs par état
| État | Scan matrice | TX touche | WiFi (ESP-NOW RX) | Heartbeat NRF | NRF / trackpad |
|---|---|---|---|---|---|
| **ACTIVE** | 1000Hz (1 ms) | immédiate (événement) | **on** (status live) | 100 ms | on |
| **THROTTLE** | ~50Hz (20 ms) | immédiate | **on** (status ~live ; palier court) | ~500 ms–1 s | on |
| **SLEEP** | stoppé | — | **off** (`esp_wifi_stop`) | stoppé | **NRF power-down (PWR_UP=0, CE low) ; trackpad off** |

Décisions de défaut : seuils **3 s / 30 s** ; WiFi **on** en THROTTLE (le palier
est transitoire et court, on garde le status quasi-live ; le gros gain WiFi vient
du SLEEP). Tous ajustables.

## Wake-on-touche (Phase 2, le morceau dur)

Topologie ROW2COL (rows = outputs pilotées, cols = inputs sensées). Avant
`esp_light_sleep_start()` :
1. Stopper le scan `keyboard_button` (pas de support sleep natif → stop/restart
   ou gérer les GPIO matrice nous-mêmes pendant le sommeil).
2. Driver **toutes les rows** au niveau actif → une touche enfoncée tire sa
   colonne à ce niveau.
3. `gpio_wakeup_enable()` sur les **7 GPIO colonnes** (niveau = niveau actif) +
   `esp_sleep_enable_gpio_wakeup()`.
4. `esp_light_sleep_start()`.

**Light-sleep (pas deep)** : tout GPIO digital peut réveiller (y compris les pins
JTAG GPIO39/42 utilisées en colonnes), RAM + config périphériques retenues →
réveil rapide, pas de reboot. Au réveil : restaurer le scan, re-scan immédiat →
la touche encore enfoncée est détectée et envoyée. Risque résiduel : un tap
ultra-bref relâché avant la fin du réveil (~ms) pourrait être manqué — acceptable,
à documenter.

## WiFi/ESP-NOW + NRF au sleep

- **Entrée SLEEP** : `esp_wifi_stop()` (gain batterie principal) ; NRF →
  power-down (registre PWR_UP=0, CE bas).
- **Réveil** : `esp_wifi_start()` + `espnow_reload_peers()` (re-dérive + applique
  le canal, ré-enregistre le peer dongle) ; NRF power-up ; reprise du heartbeat ;
  état → ACTIVE.
- Le **dongle est inchangé** : il continue de pousser `EN_INFO_STATUS` toutes les
  5 s à l'aveugle ; pendant le sommeil de la moitié, les pushes sont ratés
  (e-ink gelé, voulu). Au réveil, le prochain push rafraîchit l'affichage.

## Lien dongle pendant l'inactivité

En SLEEP le heartbeat est coupé → le dongle marque ce half "link down" après son
timeout (invisible : le half dort). La **1ère frappe** (`PKT_KEY`, auto-ACK NRF)
est reçue par le dongle quel que soit l'état link-quality ; le metric se rétablit
quand le heartbeat reprend au réveil.

## Gestion d'erreur / robustesse
- Échec `esp_wifi_start()` au réveil → log + retry ; ne pas bloquer le scan/TX
  (taper doit marcher même si le WiFi ne revient pas).
- Ne jamais entrer en SLEEP si une touche est enfoncée (l'état pressé doit
  d'abord se relâcher) — sinon risque de rater le relâchement.
- Le retour ACTIVE doit être **atomique côté scan** : restaurer le scan AVANT de
  rallumer WiFi, pour ne pas retarder la 1ère frappe.

## Tests
- **Host (TDD, `test/test_half_power.c`)** : `half_power_next()` à toutes les
  bornes — idle juste avant/après T_THROTTLE et T_SLEEP ; activité récente →
  ACTIVE ; monotonie des transitions. Ajouté à `test/CMakeLists.txt` +
  `test/test_main.c`.
- **Hardware (ajouts à `docs/HARDWARE_SMOKE_TEST.md`, section Half)** :
  - frappe reste snappy en usage (ACTIVE) ;
  - après ~30 s d'inactivité, la moitié entre en SLEEP (mesurer la chute de
    courant si banc dispo) ;
  - 1ère frappe après SLEEP : réveille **et** s'enregistre (pas de touche perdue) ;
  - e-ink se rafraîchit après réveil (status revient live) ;
  - les deux moitiés indépendamment (left + right).

## Phasage (le plan d'implémentation découpera)
1. **Phase 1 — THROTTLE** : module `half_power` + `half_power_next` (+ tests host)
   + palier THROTTLE (scan 1000→50Hz, heartbeat ralenti). **Zéro risque** (pas de
   sleep, CPU/WiFi restent on). Livre déjà le rythme dynamique.
2. **Phase 2 — SLEEP** : light-sleep + wake-on-GPIO matrice + cycle
   `esp_wifi_stop/start` + NRF power-down/up + e-ink gelé. Le morceau dur.

## Hors scope (YAGNI)
- Mesure batterie (autre "battery brick" ; les états power l'alimenteront plus
  tard pour un affichage SoC).
- Power-management du dongle (USB, incompressible).
- Deep sleep (perd l'état RAM/périphériques → reboot, trop lent/lossy pour un
  clavier).

## Critères de succès
- En usage, latence de frappe inchangée (scan 1000Hz, TX événementielle).
- Après inactivité prolongée, la moitié dort (WiFi off + NRF power-down + CPU
  light-sleep) et le courant chute nettement.
- La 1ère frappe après sommeil réveille la moitié et est transmise au dongle
  sans perte perceptible.
- Le status e-ink reste lisible (gelé) en sommeil et redevient live au réveil.
- `half_power_next()` couverte par des tests host à toutes les bornes.

## Risques de faisabilité (à lever en implémentation)
- `keyboard_button` : stop/restart propre (ou gestion directe des GPIO matrice
  pendant le sommeil) — pas de support sleep natif.
- Latence + fiabilité du cycle `esp_wifi_stop`/`esp_wifi_start` + esp_now.
- Chute de conso réelle **seulement si** NRF (et trackpad) sont aussi mis en
  veille, pas uniquement le CPU/WiFi.
