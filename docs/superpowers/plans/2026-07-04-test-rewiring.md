# Plan — Rewiring des suites « test-the-copy » sur le vrai code

**Origine** : review qualité des tests (tripwire test-review, 2026-07-04).
**Problème** : ~40 % de la suite host ne linke pas le code sous test — elle
**réimplémente la logique** ou **recopie les `#define`** dans le fichier de test,
puis asserte sur sa propre copie. Ces tests passent au vert même si le firmware
est cassé (fausse assurance). Les quick wins (commit `5583538f`) ont déjà traité
`ghost_filter` (code fantôme), les commentaires menteurs et les compteurs
fabriqués. Reste le gros morceau : reconnecter les suites au vrai code.

## Principe de correction (identique pour chaque suite)

1. Ajouter le `.c` de prod à `test/CMakeLists.txt` (`add_executable`).
2. Supprimer du fichier de test : les `#define` recopiés, les `typedef`
   dupliqués, et les fonctions qui réimplémentent la logique.
3. `#include` le vrai header ; appeler les **vraies** fonctions.
4. Réassertion sur les sorties réelles (pas sur des valeurs que le test s'est
   assignées).
5. Ajouter les cas limites listés par la review (bornes timing, overflow,
   chemins d'erreur) — TDD : écrire le cas, le voir échouer/passer contre le
   vrai code.
6. `./scripts/check.sh --host-only` vert + mettre à jour `.tripwire-testcount`
   (le compte va **monter**, l'auto-update le gère).

⚠️ **Attendu** : certaines suites vont révéler de **vrais bugs** une fois
connectées (ex. `test_leader` valide aujourd'hui un matcher plus propre que le
`leader.c` qui ship — le brancher peut faire apparaître un bug réel du matcher).
C'est le but. Traiter chaque bug révélé en systematic-debugging, séparément.

## Lot 1 — Input (priorité haute : logique keystroke cœur)

Risque de régression le plus élevé ; c'est là que le gel/retrait de features
(cf. double-tap CapsLock) doit être gardé.

| Suite | Linker | Vraies fonctions à exercer | Pièges |
|---|---|---|---|
| `test_tap_hold.c` | `tap_hold.c` | `tap_hold_on_press/on_release/tick/interrupt/consume_tap`, `get_active_mods/layer` | timeout `TAP_HOLD_TIMEOUT_MS`, `find_free()` exhaustion → besoin d'un `now_ms` injectable |
| `test_tap_dance.c` | `tap_dance.c` | `tap_dance_on_press` (N fois), `tick`, `consume`, clamp `TAP_DANCE_MAX_TAPS` | HOLD→action[3], slot non configuré, timeout |
| `test_combo.c` | `combo.c` | `combo_process`, `combo_defer_key`, `is_combo_key`, `combo_should_defer` | timeout `COMBO_TIMEOUT_MS`, `MAX_DEFERRED` exhaustion |
| `test_leader.c` | `leader.c` | `leader_start/feed/tick/consume` | ⚠️ matcher lookahead `sequence[j+1]` suspect — peut révéler un bug ; buffer-full cancel `LEADER_MAX_SEQ_LEN` |
| `test_key_features.c` | `key_features.c` | `caps_word_process/toggle`, `osm_*`, `osl_*`, `repeat_key_*` | brancher les vraies statics ; couvrir `kc==0`, non-lettre→deactivate |
| `test_key_stats.c` | `key_stats.c` | `key_stats_record_press`, `key_stats_check_save` | wraparound uint32, intervalle de save mockable |
| `test_bigram.c` | `key_stats.c` | `key_stats_record_press` + `get_bigram_stats_max` | garde `last_key_idx < NUM_KEYS` ; saturation `UINT16_MAX` (déjà bon intent) |
| `test_macro_seq.c` | (via `key_processor.c`, déjà linké par keycode_report) | `expand_macro` | `MACRO_DELAY_MARKER`, fallback legacy `steps[0]==0`, out-of-range |
| `test_keycodes.c` | (header only) | `#include "key_definitions.h"`, supprimer les defines recopiés | couvrir layer 10-15, nibble mods |
| `test_matrix_constants.c` | (headers) | inclure les vrais headers au lieu de redéfinir | — |
| `test_keymap_nvs.c` | `keymap.c` + fake `nvs_flash` | `save_keymaps/load_keymaps/load_macros` | garde de size-mismatch `keymap.c:162` ; struct `macro_t` réelle (a un champ `steps[]`) |

## Lot 2 — Autres domaines

| Suite | Linker | Vraies fonctions | Pièges |
|---|---|---|---|
| `test_tama_engine.c` | `tama/tama_engine.c` | `tama_engine_*`, `update_state`, `xp_for_level`, `get_critter` | hystérésis `STATE_HOLD_MIN=50` omise par la copie ; XP scaling `+250/niveau` ; clamp critter >19 |
| `test_led_anim_constants.c` | extraire la courbe pure de `led_strip_anim.c` puis linker | `anim_reactive`, `anim_kpm_bar` | tautologies sur `#define` à supprimer |
| `test_en_status.c` | déplacer `build_link_label`/`build_usb_label` dans un `.c` host-safe, linker | les 2 builders réels | le codec `en_encode/decode_status` est déjà réel (OK) |
| `test_cdc_protocol.c` | `cdc/cdc_binary_cmds.c` (ou l'encodeur extrait) | encodeurs KEYSTATS/BIGRAMS/entry réels | sinon supprimer le fichier (tautologie totale) ; `MACRO_1` inventé à retirer |

## Ordre d'exécution recommandé

1. **Lot 1 input** d'abord (regression cœur), suite par suite, board vert entre chaque.
2. **Lot 2** ensuite.
3. Après rewiring : envisager un **mutation testing** C host (mutation manuelle
   ciblée ou outil type `mull`) pour confirmer que les tests mordent vraiment.

## Gaps de couverture à ajouter dans les *bons* tests (indépendant du rewiring)

- `openpgp_card`/crypto : KATs RFC-6979/7748 + cas négatifs (scalaire nul, d≥N,
  point d'ordre faible) contre le vrai `openpgp_crypto.c`. *(basse prio : PGP gelé)*
- `cdc_rx_feed` : cmd inconnue → `ERR_UNKNOWN`, len==4096 borne, 2 frames/buffer.
- `apdu` : Case 3E/4E extended-Lc + reject tronqué.
- `otp_proto` : chemin refus `confirm==2` → report zéro + reset.
- `cfg_bridge` : gardes réassembleur (total incohérent, slice sur-taille, overflow).
- `cr_crc16` : un KAT sur entrée fixe (`"123456789"`).

## Definition of Done (par suite)

- Le `.c` de prod est compilé dans le runner.
- Zéro `#define`/`typedef`/fonction recopiés du code de prod dans le test.
- Chaque assertion porte sur une sortie produite par le vrai code.
- Les cas limites listés sont couverts.
- `./scripts/check.sh` vert (6 boards) ; `.tripwire-testcount` mis à jour.
