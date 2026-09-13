# Plan d'exécution — câblage runtime de la fusion (phase 1, suite)

Design : `2026-09-12-dongle-fusion-deux-moteurs-design.md`.
Plan cadre : `2026-09-12-dongle-fusion.md`. Ce document détaille les ÉTAPES
runtime, celles qui changent le comportement à l'exécution (le compile-only est
fait : le moteur lie déjà dans le dongle, commit `48bdb2c3`).

Règle d'or inchangée : tout derrière `KASE_DONGLE_FUSION` (défaut off), le
clavier actuel marche à chaque étape, `check.sh` vert. Chaque étape se vérifie à
la COMPILATION (build dongle+fusion / tous boards+fusion) ; la validation « ça
tape » demande le banc et les trois cartes (étape D).

## Modèle de données du moteur (établi 2026-09-13)
`build_keycode_report()` lit `current_press_row/col[6]` + `keymaps[layer][r][c]`.
Sur le maître : le scan remplit `current_press[0..local]`, `matrix_apply_remote()`
ajoute le distant (cols 7-13, miroir), puis `build_keycode_report()` + `send_hid_key()`.
Au dongle, les DEUX moitiés sont distantes → on remplit tout `current_press[]`
depuis les deux bitmaps (c'est `fusion_collect`), puis moteur, puis `send_hid_key()`.
`send_hid_key()` → `hid_send_keyboard()` → USB dongle.

Globales que `matrix_scan.c` fournit sur un clavier et qui manquent au dongle :
`MATRIX_STATE`, `SLAVE_MATRIX_STATE`, `keycodes`, `current_press_{row,col,stat}`,
`stat_matrix_changed`, `last_layer`, `is_layer_changed`, `last_activity_time_ms`.
(`matrix_test_*`, `current_layout` déjà dans `dongle_state.c` ; `extra_keycodes`,
`prev_press_*` dans `key_processor.c`.)

## Étape A — cœur RX : le dongle fait tourner le moteur  ⏳
- **A1** `main/comm/rf/dongle_engine.{c,h}` (compilé sous `KASE_DONGLE_FUSION`) :
  - définit les globales d'état moteur manquantes (liste ci-dessus) ;
  - `dongle_engine_feed(fusion_state_t*, cols, mirror)` : remplit `current_press_*`
    via `fusion_collect`, lève `stat_matrix_changed` ;
  - tâche `dongle_engine_task` : tick ~10 ms (`tap_hold_tick`/`tap_dance_tick`),
    et sur changement/hold/tap-dance/leader/macro → `build_keycode_report()` +
    `send_hid_key()` (calqué sur le corps de `keyboard_task.c`) ;
  - `dongle_engine_start()`.
- **A2** `rf_rx_task.c` `drain_radio` : sous fusion, `PKT_TYPE_MATRIX` →
  `fusion_apply` ; expiration via `fusion_timeout` ; NE relaie PLUS le HID fini
  en mode fusion. Démarrer `dongle_engine_start()` dans `rf_rx_start()`.
- **A3** build `kase_dongle`+fusion vert.

## Étape B — TX : les moitiés émettent leur brut au dongle

**Adressage tranché (2026-09-13)** : les DEUX moitiés visent le même slot clavier
du dongle (adresse suffixe 0x01), distinguées par l'identité de moitié dans
PKT_TYPE_MATRIX. La droite réutilise la pile `kbd_relay`/appairage de la gauche.
Collisions ESB rares au débit clavier, acceptées. (vs. un 2e pipe dédié, écarté.)

**Primitif commun** : `kbd_relay_send_matrix(half, bitmap)` → encode PKT_TYPE_MATRIX
et passe par `kbd_tx_locked()` (le chemin d'excursion/direct déjà éprouvé).

- **B2** `niphar_left` sous fusion ✅ **compile-only fait** : émet
  `rf_matrix(RF_HALF_LEFT)` au dongle sur changement (bloc fusion dans
  matrix_scan.c) et ne relaie PLUS le HID fini (les sites kbd_relay_send_kbd de
  hid_report.c gardés `!FUSION`). Vérifié au lien : `kbd_relay_send_matrix` lié,
  `kbd_relay_send_kbd` éliminé. `kbd_relay` reste initialisé (send_matrix passe
  par son `kbd_tx_locked`). RESTE AU BANC : `HALF_LINK_RX` off (cesser d'écouter
  la droite — gain d'autonomie), la réaffirmation périodique des maintiens (contre
  le timeout du dongle), et le routage USB-gauche (phase 2).
- **B1** `niphar_right` sous fusion ✅ **compile-only fait** : `half_link_tx`
  retargeté vers le SLOT CLAVIER DU DONGLE (canal RF_CH_KBD_DONGLE, suffixe
  RF_ADDR_KBD_DONGLE, adresse dérivée du set_id d'appairage NVS), et le payload
  passe en `rf_matrix(RF_HALF_RIGHT)`. La tâche de rafraîchissement existante
  donne gratuitement la réaffirmation des maintiens. Vérifié au lien
  (niphar_right+fusion) : `rf_encode_matrix` lié, `rf_encode_heartbeat` éliminé.
  RESTE AU BANC : **le handshake d'appairage droite↔dongle**. La droite n'a
  jamais été appairée au dongle ; sans set_id en NVS l'adresse reste d'usine et
  le dongle n'acquitte pas. Le handshake (REQ/ACK sur le rendez-vous, comme
  kbd_pairing_task) se valide contre des ACK réels — c'est LA seule pièce
  runtime restante de la topologie B.
- **B3** build tous boards + fusion vert : dongle ✅, gauche ✅, droite ✅
  (les trois compilent et lient en fusion).

⚠ **B inverse la propriété de la radio sur chaque moitié** (HALF_LINK_RX/TX
basculent, la droite s'appaire au dongle). C'est la zone que CLAUDE.md signale
comme ayant échoué **trois fois en silence** (« une puce, un propriétaire » ;
« émettre/relâcher ne composent pas »). « Compile vert » n'y prouve rien : un
mauvais canal/adresse/propriétaire est muet. **À écrire avec le banc dans la
boucle**, chaque changement RF vérifié contre des ACK réels et un flash des trois
cartes — pas à l'aveugle en fin de session.

## Étape C — supervision / repli
- Batterie : les moitiés continuent `PKT_TYPE_STATUS` ; le dongle l'a déjà.
- Perte de lien : `fusion_timeout` relâche par moitié (déjà testé host).

## Étape D — banc (Mae)
Flasher les 3 cartes en fusion, prouver que les deux moitiés tapent via le moteur
du dongle. Puis phase 2 (gauche autonome USB) et phase 3 (config répliquée).
