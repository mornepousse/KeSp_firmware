# Rewiring des tests host « fausse assurance » — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Les ~15 suites qui testent une copie locale de la logique linkent le vrai module de prod ; chaque suite rewirée prouve qu'elle mord (sabotage transitoire → rouge) ; `ghost_filter` (code fantôme) supprimé.

> ## ✅ AVANCEMENT (2026-07-04)
> - **Phase 0** (quick wins) FAIT : ghost_filter supprimé, commentaires menteurs
>   corrigés, compteurs board_config réels (commits 5583538f / 1f23bf76).
> - **Phase 2 / T6a** FAIT : `keycodes` + `matrix_constants` sur vrais headers
>   (commit 3a31ccb8). Reste **T6b** (`led_anim_constants`).
> - **Phase 1 COMPLÈTE** — stratégie **intégration** choisie (les stubs de
>   `test_keycode_report.c` retirés, il tourne sur les vrais modules) :
>   - T5 `key_features` (dd54fa87), T1 `tap_hold` (2c8247ab, + stub `esp_timer`),
>     T2 `tap_dance` (bdf5f0c3, + `host_clock` partagé), T3 `combo` (ba30723a),
>     T4 `leader` (63d8f6c4). Preuve de morsure sur chaque.
>   - ⚠️ **T4 a RÉVÉLÉ UN VRAI BUG de prod** (corrigé, 63d8f6c4) : une séquence
>     leader préfixe d'une autre l'occultait (`[A]` rendait `[A,B]` inatteignable).
>     Fix : différer la résolution tant que le buffer est préfixe d'une séquence
>     plus longue ; matcher réécrit propre (supprime aussi un OOB read latent).
> - Infra posée : `test/esp_timer.h`, `test/host_clock.{c,h}` (horloge injectable).
> - **Reste** : T6b (led_anim), Phase 3 (cas à instruire : cdc_protocol,
>   tama_engine, en_status, key_stats/bigram, keymap_nvs, macro_seq), Phase 4
>   (gaps de couverture des bons tests).

**Architecture:** Le runner host (`test/CMakeLists.txt`) compile déjà des `.c` de prod (pattern établi : `../main/comm/rf/heartbeat.c` etc.) avec stubs host (`test/esp_log.h`, `test/freertos/`). Chaque tâche = une suite : ajouter le vrai `.c` (ou header) au runner, supprimer la copie locale du test, appeler les vraies fonctions, prouver la morsure.

**Tech Stack:** C host (CMake), harnais `test_runner` existant, tripwire v0.9.0 (ratchet 1223, garde d'assertions sur `test/`).

## Contexte source (extrait du /tripwire:test-review du 2026-07-04)

Suites ❌ (testent une copie) : keycodes, key_features, keymap_nvs, tap_hold,
tap_dance, combo, leader, macro_seq, key_stats, bigram, matrix_constants,
cdc_protocol, tama_engine, led_anim_constants, en_status, board_config,
ghost_filter. Cas graves : ghost_filter teste un algo qui ne ship nulle part ;
leader valide un matcher plus propre que la prod ; board_config incrémente
`_test_pass_count++` sans assertion ; commentaires « TDD red state » périmés
dans test_rf_packet.c:160 / test_rf_pairing.c:134.

## Doctrine d'exécution (économie + anti-hallucination)

- **Implémenteurs : modèle économique (haiku)** — chaque tâche est mécanique
  et spécifiée, et TROIS oracles rattrapent toute dérive : (1) compilation du
  runner, (2) le **sabotage transitoire** obligatoire (voir recette), (3) le
  ratchet 1223 + la garde d'assertions du scaffold v0.9.0 (baisse d'asserts
  dans `test/` = avertissement immédiat ; baisse du compte = rouge au push).
- **Reviewer par tâche : modèle mi-gamme (sonnet)** ; **revue finale : modèle
  fort**. Le jugement ne descend jamais en gamme.
- Extraction/lecture en greps ciblés (rtk compresse s'il intercepte).

## Global Constraints

- `./scripts/check.sh --fast` (alias `--host-only`) DOIT être vert à la fin de
  chaque tâche — c'est l'oracle. Le nombre total d'assertions ne doit jamais
  baisser (ratchet committé : `.tripwire-testcount` = 1223 ; les hausses
  s'auto-committent avec la tâche).
- **Recette de rewiring** (chaque suite) :
  1. Ajouter le vrai `.c` au runner (`test/CMakeLists.txt`, pattern existant
     `../main/...`) — ou remplacer la copie de constantes par l'include du
     vrai header.
  2. Supprimer la réimplémentation/copie locale dans le `test_*.c` ; appeler
     les vraies fonctions/constantes. Conserver ou renforcer chaque assertion
     existante (jamais en supprimer sans équivalent plus fort).
  3. Stubs manquants : compléter `test/` (pattern `esp_log.h` existant) —
     `esp_timer.h` sera nécessaire pour les modules input ; `nvs_utils.h` a
     besoin d'un shim host (table en RAM).
  4. **Preuve de morsure (OBLIGATOIRE, c'est l'acceptation)** : introduire un
     bug transitoire dans le module de prod (inverser une condition, décaler
     une borne), lancer le runner → la suite DOIT être rouge ; revert ; vert.
     Coller la preuve (sortie rouge) dans le rapport de tâche.
  5. Commit (un par suite) : `test(<suite>): rewire sur <module réel> (+preuve de morsure)`.
- Ne JAMAIS modifier le comportement d'un module de prod (sauf le sabotage
  transitoire, reverté). Si un test révèle un vrai bug de prod : STOP, le
  signaler, ne pas « adapter » le test.

---

### Phase 0 — Quick wins (1 tâche)

- [ ] **T0** : supprimer `test/test_ghost_filter.c` (algo fantôme — `matrix.c`
  n'existe pas, l'anti-ghosting réel est par timing dans `keyboard_button`) +
  retirer du CMakeLists + corriger les commentaires menteurs « doit ÉCHOUER À
  LA COMPILATION » de `test_rf_packet.c:160` et `test_rf_pairing.c:134` +
  supprimer les `_test_pass_count++` sans assertion de `test_board_config.c`
  (les remplacer par de vraies assertions sur les vrais `boards/*/board.h`).
  Note ratchet : la suppression de ghost_filter fera BAISSER le compte
  d'assertions — c'est le cas assumé prévu par le design : mettre à jour
  `.tripwire-testcount` DANS LE MÊME COMMIT (diff visible, motivé).

### ⚠️ Blocker Phase 1 — collision de stubs (découvert 2026-07-04)

`test_keycode_report.c` compile `key_processor.c` et **définit lui-même les
stubs host** de tous les modules input que key_processor appelle : `osm_arm/
consume/is_active`, `osl_get_layer/consume`, `caps_word_toggle/is_active/
process`, `repeat_key_record/get`, `wpm_record_keypress` (≈ lignes 172-208) —
et idem pour `tap_hold`/`tap_dance`/`combo`/`leader`. Donc **ajouter le vrai
`key_features.c` (ou `tap_hold.c`, …) au runner → erreur linker « multiple
definition »** (vérifié). Les tâches T1-T5 ne sont **PAS indépendantes** :
chacune exige de retirer les stubs correspondants de `test_keycode_report.c`,
ce qui fait basculer cette suite sur les vrais modules (elle devient un test
d'intégration et ses propres tests osm/osl doivent piloter le vrai état via
l'API au lieu des globales `g_osm_pending`/`g_osl_layer`). Alternative : sortir
tous ces stubs dans **un seul TU opt-in** partagé.

**Décision requise avant T1** : (a) `test_keycode_report` en intégration (vrais
modules) — moins de stubs, mais réécriture de ses tests osm/osl ; ou (b) stubs
centralisés dans un TU dédié, chaque suite choisissant réel-vs-stub. Phase 2
(constantes, sans linking) n'est PAS concernée par ce blocker.

Note deps confirmées (T5 `key_features.c`) : `wpm`/`layer_lock` sont purs ou
tirent des externs déjà fournis par `key_processor.c` ; seul `key_override`
tire NVS → guarder ses appels sous `#ifndef TEST_HOST` (pattern `sec_store.c`),
`esp_timer` non requis pour key_features.

### Phase 1 — Modules réels existants, 2-3 includes IDF (5 tâches, les plus rentables)

| Tâche | Suite | Module réel | Stubs |
|---|---|---|---|
| T1 | `test_tap_hold.c` | `main/input/tap_hold.c` | esp_log ✓, esp_timer à créer |
| T2 | `test_tap_dance.c` | `main/input/tap_dance.c` | idem |
| T3 | `test_combo.c` | `main/input/combo.c` | idem + nvs_utils shim |
| T4 | `test_leader.c` | `main/input/leader.c` (le matcher lookahead RÉEL, pas la version propre) | idem |
| T5 | `test_key_features.c` | `main/input/key_features.c` | idem |

(T1 crée le stub `esp_timer.h` + le shim `nvs_utils` ; T2-T5 les réutilisent.)

### Phase 2 — Suites « constantes » : inclure le vrai header (1 tâche groupée)

- [x] **T6a** ✅ (commit 3a31ccb8) : `test_keycodes.c` → `key_definitions.h` réel
  (valeurs recopiées = OK, pas de dérive ; +couverture LT 10-15) ;
  `test_matrix_constants.c` → `matrix_scan.h` réel (INVALID_KEY_POS +
  STORAGE_NAMESPACE). `MAX_REPORT_KEYS` reste local (aucun header source —
  dupliqué dans 5 `.c`) avec commentaire honnête. 1757/0.
- [ ] **T6b** : `test_led_anim_constants.c` → `main/led/led_strip_anim.h` (ou
  extraire la courbe pure). Supprimer les `#define` recopiés + la tautologie
  `REACTIVE_DECAY_MS > REACTIVE_ATTACK_MS`. Preuve de morsure : changer une
  constante réelle → rouge.

### Phase 3 — Cas à instruire d'abord (3 tâches, chacune commence par 10 min d'investigation citée)

- [ ] **T7** : `test_key_stats.c` + `test_bigram.c` — `key_stats.c` réel existe
  (5 includes IDF, shim nvs requis) ; localiser où vit la logique bigram
  (probablement dans `key_stats.c` ou `key_processor.c`) et rewirer dessus.
- [ ] **T8** : `test_tama_engine.c` — rewirer sur `main/tama/tama_engine.c` et
  couvrir les comportements réels omis (hystérésis `STATE_HOLD_MIN=50`,
  scaling XP `+250/niveau` — cités par le review).
- [ ] **T9** : `test_keymap_nvs.c`, `test_macro_seq.c`, `test_cdc_protocol.c`,
  `test_en_status.c` — pas de module autonome évident : localiser la logique
  réelle (keymap.c ? key_processor.c ? main/comm/cdc/*.c ? espnow_link.c —
  déjà compilé dans le runner pour peer_filter). Pour chaque : rewirer si un
  point d'entrée testable existe, sinon documenter pourquoi (fonction statique
  enfouie → proposer l'extraction en tâche future, ne pas forcer).

### Phase 4 — Gaps de couverture dans les *bons* tests (repris du plan de session, 1 tâche)

- [ ] **T10** : ajouter dans les suites déjà fiables (indépendant du rewiring) :
  `cdc_rx_feed` (cmd inconnue → `ERR_UNKNOWN`, borne len==4096, 2 frames/buffer) ;
  `apdu` (Case 3E/4E extended-Lc + reject tronqué) ; `otp_proto` (chemin refus
  `confirm==2` → report zéro + reset) ; `cfg_bridge` (gardes réassembleur :
  total incohérent, slice sur-taille, overflow) ; `cr_crc16` (KAT sur
  `"123456789"`). *(openpgp_crypto : basse prio, PGP gelé — hors périmètre.)*

### Definition of Done (par suite rewirée — reprise du plan de session)

- Le `.c` de prod est compilé dans le runner ; zéro `#define`/`typedef`/fonction
  recopiés ; chaque assertion porte sur une sortie du vrai code ; les cas
  limites listés sont couverts ; **preuve de morsure fournie**.

### Phase finale

- [ ] **TF** : `./scripts/check.sh` full (6 boards) vert ; `.tripwire-testcount`
  cohérent et committé ; re-lancer `/tripwire:test-review` sur `test/` (mode
  gros scope) et vérifier que les 14 suites sont passées de ❌ à ✅ ; commit
  récapitulatif + push (pre-push strict).

## Self-review

- Chaque suite ❌ du review a une tâche (T0-T9) ; les cas sans module autonome
  ont une sortie honnête (documenter, pas forcer). La preuve de morsure est
  l'acceptation de CHAQUE rewiring — c'est l'anti-« fausse assurance » même.
- Le ratchet gère les deux sens : hausse auto-committée, baisse (T0) assumée
  dans le commit.
