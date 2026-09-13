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

## Étape D — banc (Mae)  ✅ FAIT le 2026-09-13
Les 3 cartes flashées en fusion (app-only, appairages préservés). Résultat :
- Gauche : tape via le moteur du dongle (« qwertasdfg »).
- Droite : appairée au dongle (set_id 0x9044, épreuve 10/10 ACK), tape via le
  moteur du dongle. Miroir correct (pas de touches inversées).
- Les deux ensemble : fusionnées par le dongle (« ;lk;lsdfk…uuu »).

Appairage de la droite : `KS_CMD_RF_PAIR_START` envoyé au dongle (CDC, ttyACM0,
trame `4B 53 B2 01 00 00 00`), la droite a REQ pendant la fenêtre → ACK → NVS →
reboot appairée. **La fusion complète en RF fonctionne.**

## Reste (raffinements, pas bloquants)
- ✅ **Autonomie gauche FAITE (2026-09-13)** : `KASE_HALF_LINK_RX depends on
  !KASE_DONGLE_FUSION` → sous fusion la gauche n'écoute plus (PTX pur via
  kbd_relay), ~13 mA économisés. Validé au banc (« aoeusnth », les deux côtés
  tapent, aucune n'écoute). C'est le gain d'autonomie de la réarchitecture.
- **Réaffirmation des maintiens** : régler la cadence contre RF_LINK_LOST_MS du
  dongle (un maintien long ne doit pas être relâché). La tâche de rafraîchissement
  de half_link (droite) le fait déjà ; vérifier la gauche.
- **Comptabilité d'appairage du dongle** : la droite déclarant 0x01 écrase le MAC
  de la gauche sur ce slot (affichage seulement ; l'adresse reste correcte).
  Modèle 2-slots à affiner si on veut un pair-list exact.
- Phase 2 (gauche autonome USB) et phase 3 (config répliquée + trackpad).

## Phase 2 — gauche autonome en USB (démarrée 2026-09-13)
- ✅ **Logique de routage** (pure, testée) : `fusion_route.h` — règle 3 encodée,
  invariant « jamais gauche ET dongle qui tapent » vérifié host.
- ✅ **Émission brute route-aware** : la gauche n'alimente le dongle que hors USB
  (`fusion_left_emits_raw`) — en USB elle tapera en local, sans doubler.
- ⚠ **BLOQUÉ MATÉRIEL** : l'USB natif de la gauche (ESP32-S3, GPIO19/20, derrière
  le hub CH334R) n'énumère PAS (`cafe:4003` absent ; seuls le hub + CH340
  console remontent). TinyUSB monte pourtant côté firmware (log de boot). Phase 2
  (« la gauche tape par son USB ») exige ce chemin data — à élucider au banc
  (connecteur natif séparé ? câblage GPIO19/20 → hôte ?).
- Reste ensuite (RF, au banc) : la gauche ré-écoute en mode USB, le dongle
  **réémet la droite → gauche** et se tait, annonce RF du mode USB de la gauche.

### Cas simultané — ✅ FAIT et validé au banc le 2026-09-13
Les deux moitiés tapent par l'USB de la gauche (la droite réémise par le dongle,
fusionnée par la gauche), le dongle se tait, pas de double frappe, et les
transitions USB↔sans-fil basculent proprement dans les deux sens (radio gauche
PTX↔PRX dynamique, `rf_driver_set_ptx`/`rearm_rx`). **Phase 2 complète.**
Implémenté aux étapes 1-3 (dongle : annonce, silence, réémission) + 4a (bascule
radio dynamique + écoute) + 4b (fusion locale + HID). Commits jusqu'à 5f26c779.

### Cas simultané (gauche USB + dongle + droite) — plan RF (réalisé)
Logique de décision : DÉJÀ faite et testée (`fusion_route.h`). Reste le plumbing,
tout dans la zone à échec silencieux (« une puce, un propriétaire ») → **banc
dans la boucle, ACK réels à chaque pas**. Étapes :
1. **Annonce du mode** : en USB, la gauche cesse d'émettre sa matrice et envoie à
   la place une trame « mode=USB » périodique au dongle (nouvelle trame ou flag
   sur STATUS). Le dongle la reçoit → `fusion_dongle_types(true)`=false (il se
   tait) + `fusion_dongle_reemits(true)`=true.
2. **Réémission droite→gauche** : le dongle, en mode USB-gauche, excursion
   PRX→PTX→PRX pour renvoyer la demi-matrice de la droite à la gauche (nouveau
   lien dongle→gauche ; adresse/canal à définir — réutiliser KaSe.03 ?).
3. **Écoute dynamique de la gauche** : ⚠ ANNULE en partie le
   `depends on !KASE_DONGLE_FUSION` de `KASE_HALF_LINK_RX` — en USB la gauche est
   sur secteur et DOIT ré-écouter (la droite réémise). Donc RX n'est plus
   compile-off sous fusion mais **dynamique par route** (PTX en sans-fil, PRX en
   USB). Refonte de la propriété radio de la gauche.
4. Fusion locale gauche : matrice locale + droite réémise → moteur → HID USB.
Chaque étape se prouve au banc (la 2 et la 3 ne se valident que contre des ACK
réels). C'est une session banc dédiée, pas du code à l'aveugle.

## Phase 3 — cohérence de config (démarrée 2026-09-13)
- ✅ **Empreinte de config** : `config_fp_crc32` (CRC-32 du blob keymap, pur,
  testé host) + CDC `KS_CMD_CONFIG_FINGERPRINT` (0x16) sur gauche ET dongle. Le
  contrôleur lit les deux → égales = synchronisées. Remplace le « numéro de
  version » manuel (empreinte de contenu, auto).
- Reste : (a) le **contrôleur écrit les deux NVS** au remappage (repo
  KeSp_controller) ; (b) **annonce RF** de l'empreinte par la gauche +
  **garde-fou** dongle (refuse de tourner sur empreinte ≠) — change une trame RF,
  donc reflash des deux, à faire au banc ; (c) étendre l'empreinte aux
  macros/combos/tap-dance si besoin.
- Provisionnement manuel en attendant : voir la mémoire
  `fusion-dongle-et-usb-gauche` (dump NVS gauche par FTDI → SETLAYER au dongle).
