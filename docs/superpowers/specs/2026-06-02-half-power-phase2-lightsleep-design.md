# Design — Phase 2 : light-sleep réel des moitiés (wake sur touche)

Date : 2026-06-02
Statut : approuvé (brainstorming) → à transformer en plan d'implémentation
Suite de : `docs/superpowers/specs/2026-06-02-half-power-management-design.md` (Phase 2 du phasage)
Pré-requis : Phase 1 livrée (`half_power` module + heartbeat throttle, commits `f22229b`/`b89314b`).

## Problème / objectif

La Phase 1 a posé la machine à états (`half_power_next`) + le throttle heartbeat,
mais le gros de la conso d'une moitié batterie reste : **WiFi allumé en continu**
(~80-120 mA), **scan matrice 1000Hz** et **CPU jamais en sleep**. La Phase 2
implémente le vrai palier **SLEEP** : light-sleep CPU + WiFi off + NRF power-down,
réveil sur appui de touche.

### Décisions (brainstorming)
- **Réveil = touches seules** (GPIO colonnes matrice). Pas de trackpad-wake, pas
  de check-in périodique. Conséquence assumée : pendant le sommeil le dongle voit
  la moitié "link down" et l'e-ink est gelé (bistable → lisible, zéro courant) ;
  tout reprend à la 1re frappe.
- **Latence 1re frappe** : quelques ms acceptables au réveil → light-sleep viable.
- **Seuil SLEEP = 15 s** (le réveil étant rapide, on dort tôt). Change
  `HALF_POWER_T_SLEEP_MS` 30000 → **15000**.
- **Light-sleep manuel** (pas esp_pm automatique) : la task de scan, aujourd'hui
  `for(;;) vTaskDelay(portMAX_DELAY)`, devient le **contrôleur d'énergie**.

## Architecture

La boucle idle de la task de scan devient le contrôleur : réveil périodique
(~500 ms), `half_power_next(s_last_activity_ms, now)` (réutilisé Phase 1), et si
**SLEEP** → séquence de sommeil (bloque dans `esp_light_sleep_start()` jusqu'au
réveil touche). Pas de nouvelle task → pas de coordination multi-task.

```
scan task idle loop (contrôleur)
  every ~500ms: state = half_power_next(last_activity, now)
    state != SLEEP -> continue
    state == SLEEP -> sleep_sequence():
        quiesce timers (heartbeat, LVGL, trackpad poll) + keyboard_button_delete
        trackpad power-down ; esp_wifi_stop ; NRF power-down
        configure matrix GPIO wake (rows driven, cols wake)
        esp_light_sleep_start()   ← blocks until key GPIO
        wake_sequence():
            NRF power-up ; keyboard_button_create -> 1st scan sends held key (NRF)
            restart heartbeat ; last_activity = now (ACTIVE)
            esp_wifi_start + espnow_reload_peers ; trackpad re-init ; resume LVGL
```

## Composants / séquences

### Séquence d'endormissement
1. `esp_timer_stop()` du heartbeat (100 ms).
2. **Quiescer TOUS les esp_timers/tasks qui réveilleraient le CPU** :
   `keyboard_button_delete()` (stoppe son scan 1 ms), pause des timers e-ink/LVGL
   et du polling trackpad. **Critique** : `esp_light_sleep_start()` se réveille au
   prochain esp_timer pending → tout timer périodique non stoppé = micro-réveils =
   zéro économie. Voir "Risque #1".
3. Trackpad → power-down.
4. `esp_wifi_stop()`.
5. NRF → power-down (PWR_UP=0, CE bas).
6. Config wake matrice : driver toutes les rows au niveau actif ;
   `gpio_wakeup_enable()` sur les 7 GPIO colonnes (pull adapté) ;
   `esp_sleep_enable_gpio_wakeup()`.
7. `esp_light_sleep_start()` → bloque jusqu'à l'appui d'une touche.

### Séquence de réveil (ordonnée pour la latence)
1. NRF power-up.
2. `keyboard_button_create()` → 1er scan détecte la touche enfoncée →
   `tx_key_event` l'envoie sur NRF. **La frappe part ici** (~quelques ms).
3. Restart heartbeat timer ; `s_last_activity_ms = now` → ACTIVE.
4. **Ensuite** : `esp_wifi_start()` + `espnow_reload_peers()` ; re-init trackpad ;
   reprise e-ink/LVGL. (WiFi = affichage seulement → après la frappe.)

### e-ink
Bistable → garde l'image à zéro courant en sommeil. Au réveil, reprise + prochain
push dongle rafraîchit. Seuls les **timers LVGL** sont mis en pause avant sommeil
(§ séquence endormissement, point 2).

### Côté dongle / lien
Inchangé. Le dongle pousse à l'aveugle (5 s) ; "link down" pendant le sommeil
(invisible). 1re frappe `PKT_KEY` auto-ACK reçue quoi qu'il arrive ; link-quality
se rétablit au réveil.

## Gestion d'erreur / robustesse
- `esp_light_sleep_start()` : vérifier la cause de réveil ; si ≠ GPIO, reboucler.
- **Ne jamais dormir si une touche est enfoncée** (attendre le relâchement) →
  sinon on rate le release au réveil (touche bloquée).
- Échec `esp_wifi_start()` au réveil → log + retry, **sans bloquer** scan/TX
  (taper doit marcher même WiFi KO).
- Le contrôleur ne ré-endort pas immédiatement après réveil (l'activité remet
  ACTIVE ; re-SLEEP seulement après 15 s d'inactivité réelle).

## Tests
- **Host** : pas de nouvelle logique pure. Seul changement testable : le seuil
  `HALF_POWER_T_SLEEP_MS` passe à 15000 → mettre à jour `test/test_half_power.c`
  (bornes 14999 → THROTTLE, 15000 → SLEEP) + l'assertion existante 30000.
- **Hardware (smoke-test, section Half)** :
  - entre en sommeil après ~15 s d'inactivité ;
  - **chute de courant mesurée** (NRF + WiFi + trackpad off) — critère de succès ;
  - 1re frappe réveille + s'enregistre (latence ressentie acceptable) ;
  - frappes suivantes plein régime ;
  - e-ink lisible gelé puis live au réveil ;
  - pas de touche fantôme/bloquée au réveil ; relâchement géré ;
  - les deux moitiés (left + right) indépendamment.

## Phasage interne du plan (ordre d'implémentation, par risque)
1. **Audit timers** (Risque #1) : recenser tous les esp_timers/tasks actifs sur la
   moitié (heartbeat, LVGL tick, trackpad poll, keyboard_button) et leurs API de
   stop/pause. Livrable : doc/inventaire dans le plan, pas de code.
2. **Seuil 15 s** : `HALF_POWER_T_SLEEP_MS` 30000→15000 + test host. Petit, isolé.
3. **Squelette contrôleur** : boucle idle → réveil ~500 ms → `half_power_next` ;
   log de transition ACTIVE/THROTTLE/SLEEP (sans sleep réel encore).
4. **Téardown/restore** (sans sleep d'abord) : stop/restart heartbeat +
   keyboard_button + WiFi + NRF + trackpad + LVGL, vérifier que tout repart bien.
5. **Light-sleep + wake GPIO** : `esp_light_sleep_start()` + config wake colonnes ;
   valider réveil sur touche + envoi de la 1re frappe.
6. **Mesure conso** + ajustements (pull, polarité, ordre).

## Critères de succès
- Après 15 s d'inactivité, la moitié entre en light-sleep (CPU + WiFi off + NRF
  power-down) et le **courant chute nettement** (mesuré).
- La 1re frappe réveille la moitié et est transmise au dongle sans perte, latence
  ressentie acceptable ; les suivantes à plein régime.
- e-ink lisible (gelé) en sommeil, redevient live au réveil.
- Aucune touche bloquée/fantôme au réveil.

## Risques de faisabilité (ordonnés)
1. **Quiescer LVGL/e-ink + tous les timers** avant sleep (sinon micro-réveils →
   zéro gain). Risque #1, audité en premier (étape 1 du plan).
2. `keyboard_button_delete/create` propre + reconfiguration GPIO matrice pour le
   wake (polarité ROW2COL : rows drivées au niveau actif, cols en wake — valider
   pull + niveau au scope).
3. Fiabilité + latence du cycle `esp_wifi_stop`/`esp_wifi_start` + esp_now.
4. Chute de courant réelle conditionnée à NRF + trackpad effectivement en veille.

## Hors scope
Trackpad-wake, check-in périodique (écartés au brainstorming) ; deep-sleep (perte
d'état RAM/périph → reboot) ; power-management du dongle (USB, incompressible) ;
mesure batterie/SoC (autre brick).
