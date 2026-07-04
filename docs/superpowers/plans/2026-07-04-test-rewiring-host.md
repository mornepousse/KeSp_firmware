# Rewiring des tests host « fausse assurance » — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Les ~15 suites qui testent une copie locale de la logique linkent le vrai module de prod ; chaque suite rewirée prouve qu'elle mord (sabotage transitoire → rouge) ; `ghost_filter` (code fantôme) supprimé.

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

- [ ] **T6** : `test_keycodes.c` → `main/input/key_definitions.h` réel ;
  `test_matrix_constants.c` → `main/input/keyboard_config.h`/`matrix_scan.h` ;
  `test_led_anim_constants.c` → `main/led/led_strip_anim.h`. Supprimer chaque
  copie de `#define`. Preuve de morsure : changer une constante réelle → rouge.

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
