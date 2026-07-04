# Audit de code — firmware KaSe (2026-07-04)

Audit de la **qualité du code de prod** (pas des tests), déclenché par le constat
que la review des tests avait révélé des suites creuses — donc des bugs de prod
pouvaient s'y cacher (le bug `leader` n'est sorti qu'en branchant les vrais
tests). Périmètre « cible chaude » : pipeline input (où les tests creux
masquaient) + entrées externes critiques (CDC, RF, NVS).

Méthode : 5 auditeurs sonnet en fan-out, findings **vérifiés adversarialement**.
Statut par finding : **[V]** = vérifié sur le code réel dans cette session ·
**[A]** = rapporté par l'agent (rigoureux, auto-vérifié), non re-vérifié à la main.

## Verdict global

- **Aucune corruption mémoire déclenchable depuis l'extérieur** (CDC/RF bornent
  tout ; le fix H-3 tient avec double bornage). Le seul trou de sûreté mémoire
  est une **lecture** OOB via une keymap auto-configurée (LT/OSL couche 10-15).
- Le vrai gain de l'audit : un **cluster de bugs de correction** dans le pipeline
  input (surtout `tap_hold`, comme `leader`) que les tests creux cachaient.

---

## 🔴 CRITIQUE

### C1 — Lecture hors-bornes de `keymaps[]` via LT et OSL (couche 10-15) **[V]**
`main/input/tap_hold.c:61-63` (LT) · `main/input/key_features.c:29` (OSL) ·
accès `main/input/key_processor.c:288, 312`

`K_LT_LAYER`/`K_OSL_LAYER` extraient un **nibble 0-15**, mais le tableau est
`keymaps[LAYERS=10][ROWS][COLS]`. MO/TO **sont** clampés
(`apply_momentary_layer` : `if (layer <= 9)`, key_processor.c:58) ; **LT et OSL
ne le sont pas** → `keymaps[10..15][r][c]` = lecture UB (keycodes arbitraires
injectés), et `current_layout` reste bloqué sur une couche illégale pendant le
hold.
**Déclencheur** : keymap contenant `K_LT(10..15, kc)` ou `K_OSL(10..15)` —
encodage valide, uploadable via CDC sans validation firmware.
**Note** : c'est une lecture (pas une écriture) → UB mais probablement pas de
fault sur ESP32 (RAM adjacente mappée). Réel néanmoins.
**Fix** : clamper `if (layer >= LAYERS) return;` dans `activate_hold` (branche LT)
et `osl_arm`, symétrique à MO. Idéalement + valider les couches côté handler
keymap CDC.

---

## 🟠 ÉLEVÉ

### E1 — `tap_hold` : deux LT tenues → clavier bloqué sur une couche **[V]**
`main/input/tap_hold.c:53-86` · globals `active_hold_layer` (l.24) + `last_layer`

`active_hold_layer`/`last_layer` sont **mono-slot** alors que
`TAP_HOLD_MAX_PENDING == 4` autorise 4 tap/hold concurrents. Scénario (LT(1)=P1,
LT(2)=P2, base=0) : P1 hold → `last_layer=0,cur=1` ; P2 hold → `last_layer=1`
(écrasé), `cur=2` ; relâche P2 → `cur=1` ; relâche P1 → `active_hold_layer==-1`
(garde l.77 rate), `cur=last_layer=1`. **Résultat : couche bloquée sur 1, aucune
LT tenue.** Le garde-fou de relâchement (key_processor.c:468) ne sauve pas
(`cur==last_layer`). **C'est le sibling direct du bug leader.**
**Fix** : couche de hold **per-entry** (stocker le `last_layer` capturé dans
`tap_hold_entry_t` + faire de `active_hold_layer` une pile), ne restaurer que le
hold LT le plus récent.

### E2 — `tap_hold` : `deactivate_hold` restaure `current_layout` sans enregistrer le « changer » (même racine que E1) **[A]**
`main/input/tap_hold.c:79` · vs voie MO `key_processor.c:63-64`

`activate_hold` écrit `current_layout` mais n'enregistre pas
`current_row/col_layer_changer` (contrairement à MO). Dès qu'une LT s'active,
key_processor.c:468 voit `cur != last_layer`, ne trouve pas de changer tenu, et
peut faire un teardown parasite. Bénin aujourd'hui (car `active_hold_layer`
pilote le vrai keymap) mais fragile. À traiter avec E1.

### E3 — NVS `load_keymaps` sans garde de taille → perte des keymaps à un upgrade **[A]**
`main/input/keymap.c:51-73`

Asymétrie avec `load_macros` (qui sonde `stored_size` avec `NULL` d'abord).
Un upgrade changeant `LAYERS`/`MATRIX_*` : stored plus grand → `nvs_get_blob`
renvoie `ESP_ERR_NVS_INVALID_LENGTH`, keymaps restent aux défauts →
**perte de tous les keymaps** (loggé seulement) ; stored plus petit → remplissage
partiel (données mixtes). Pas de crash (borné). **Fix** : pattern
`nvs_load_blob_with_total` (probe NULL → compare → skip) + octet magic/version.

### E4 — `save_*` en `void` → faux `OK` au controller quand NVS pleine **[A]**
`main/input/keymap.c:38-41` (+ `save_macros`/`save_layout_names`) ·
`main/comm/cdc/cdc_binary_cmds.c:139-142`

`save_keymaps` est `void` : sur `ESP_ERR_NVS_NOT_ENOUGH_SPACE` elle log et
retourne, puis le handler CDC répond `ks_respond_ok` **inconditionnellement**.
Namespace `"storage"` 64KB partagé (bigrams ~10KB + stats + td/combo/leader/…).
Saturé → l'utilisateur édite une touche, reçoit **OK**, rien n'est persisté,
perdu au reboot. **Fix** : `save_*` renvoie `esp_err_t`, handler répond
`ks_respond_err` sur échec.

### E5 — CDC : course sur `bin_rx` + dispatch sans re-CRC **[A]**
`main/comm/cdc/cdc_binary_protocol.c:169-345` · `usb_hid.c:303-318` ·
`cdc_acm_com.c:19,31`

`bin_rx` (static) est **écrit** par la callback RX TinyUSB et **lu** par la tâche
de dispatch (poll 50 ms) **sans mutex** (ESP32-S3 dual-core). Un hôte hostile
qui n'attend pas la réponse envoie frame B avant que A soit consommée ;
`ks_rx_feed` ne teste pas `ready`, `ks_process_one` **ne revérifie pas le CRC**
(l.336-339) → dispatch d'une commande avec payload jamais validé (mélange A/B).
**Pas OOB** (`payload_len` clampé 4096), mais corruption d'état / commande non
authentifiée (SETKEY/NVS_RESET/DFU/OTA). **Fix** : snapshot atomique
`{cmd,len,payload}` sous verrou au moment où le CRC passe + re-CRC avant dispatch
(ou refuser le feed tant que `ready`).

### E6 — Key override : ignore les mods physiques + `result_mod` jeté **[A]**
`main/input/key_features.c:177-188` · `main/input/key_processor.c:299, 356-360`

`key_override_check(kc, th_mods, …)` reçoit seulement `active_hold_mods`
(MT/OSM) — un Shift **physique** (touche `K_LSHIFT` → 0xE1 dans `keycodes[]`)
n'y entre jamais. Donc un override `Shift+,`→`;` ne se déclenche **jamais** dans
le cas normal. Et quand il matche, `override_mod` est calculé puis jeté
(`/* TODO: apply override_mod */`) sans retirer le mod déclencheur →
`Shift+,`→`;` enverrait `Shift+;` = `:`. **Fix** : combiner `th_mods` + les mods
physiques de `keycodes[]` (comme le fait `K_GESC` l.219-227), appliquer
`result_mod`, retirer le mod déclencheur.

### E7 — RF pairing : un PAIR_REQ forgé écrase le MAC d'un slot appairé + `paired_count` non borné **[A]** *(sous caveat RF non authentifié, mais persistant)*
`main/comm/rf/rf_rx_task.c:329-346` · `main/comm/rf/rf_pairing.c:66-74`

Pendant la fenêtre de pairing (2 min, user-initiée), un `PKT_PAIR_REQ` forgé
`[0xF0][MAC ×6][slot=0x01]` : `rf_pairing_resolve_slot` honore inconditionnellement
0x01/0x02 et court-circuite le garde « fenêtre pleine » → `save_peer_dongle`
**écrase `mac_left` en NVS**. Deux reqs forgés lient les deux halves au MAC
attaquant, **persistant** (survit au reboot). Un flood monte `paired_count` >2.
**Fix** : refuser un `declared_slot` déjà occupé par un MAC non nul différent
(sauf `--reset`) ; borner `paired_count` (rejeter si ≥2).

---

## 🟡 MOYEN

### M1 — RF `verify_rx` teste `0x0E` alors que la radio est en `0x06` **[V]** *(one-liner, forte actionabilité)*
`main/comm/rf/rf_driver.c:314` (+ logs :319, :323)

Init (l.219) et rearm (l.260) écrivent `RF_SETUP=0x06` (1 Mbps, avec commentaire
« MUST match … Was 0x0E ») mais `verify_rx` teste `rf_v == 0x0E` (2 Mbps, l'ancienne
valeur). Résidu exact du revert 250k→1M non propagé à la vérif. Conséquence :
`verify_rx` renvoie toujours `false`, log `verify FAIL` à chaque boot, et **masque
une vraie défaillance de config radio** (le but du check). Pas de crash (retour
non gated). **Fix** : `(rf_v == 0x06)` l.314 + corriger `exp 0x0E` dans les logs.

### M2 — `tap_dance` : résolution « 4ᵉ tap » perdue (flag effacé par le tick) **[A]**
`main/input/tap_dance.c:88-91` vs `:116`. Un 4ᵉ appui rapide (`tap_count>MAX=3`)
résout dans `on_press` (dans `process_matrix_changes`, après que le consommateur
`keyboard_task.c:73` a déjà tourné) ; le tick suivant fait `resolved_flag=false`
→ **le keycode 3-tap n'est jamais envoyé**. Fix : ne pas effacer aveuglément
`resolved_flag`, ou consommer par edge sur `resolved_keycode != 0`.

### M3 — `combo` : touches supprimées sur épuisement de slots (6KRO) **[A]**
`main/input/combo.c:158-171`. `combo_active[i]=true` posé **avant** `if (d1||d2)`.
Si 6 touches simultanées et `MAX_DEFERRED=4`, les 2 dernières (un combo) échouent
`alloc_deferred` → pas de résolution mais `combo_active` reste true →
`is_active_combo_key` supprime les deux touches tant qu'elles sont tenues.
**Perte d'entrée silencieuse.** Fix : `combo_active[i]=true` seulement dans la
branche `if (d1||d2)`.

### M4 — `tap_hold_consume_tap` s'arrête tôt sur un tap OSM **[A]**
`main/input/tap_hold.c:185-188` · `key_processor.c:410`. Le `while ((kc=consume_tap())!=0)`
ne distingue pas « OSM armé, continue » (retour 0) de « plus rien ». Relâcher OSM
+ MT/LT en tap le même cycle, OSM en premier → le tap MT/LT n'est pas émis ce
cycle. Fix : boucler dans `consume_tap` jusqu'à un tap produisant un keycode.

### M5 — OSM gaspillé par un release (pas seulement une frappe) **[A]**
`main/input/key_processor.c:388` vs `:454`. Step 5 fait `osm_consume()`
inconditionnellement ; l'OSL, lui, est gardé sur `has_normal_press`. Relâcher une
touche entre le tap OSM et la frappe cible consomme l'OSM dans le vide. Fix :
garder l'OSM sur `has_normal_press`.

### M6 — MO potentiellement bloqué si la touche MO est MO sur sa propre couche **[A, à vérifier keymap]**
`main/input/key_processor.c:285-290, 458`. Step 2 lit depuis `base_layer=current_layout`.
Si `MO_Lx` est mappé sur la couche 0 **et** sur la couche x, au cycle suivant
`last_layer==current_layout==x` → Step 10 sauté → couche jamais restaurée.
Dépend du contenu keymap (touche de couche transparente `K_NO` sur sa couche ?
→ pas de bug). À confirmer côté éditeur/défauts.

### M7 — Mods perdus silencieusement si report plein (6 touches) **[A]**
`main/input/key_processor.c:399-405`. La boucle d'injection des mods abandonne
sans warning si `keycodes[0..5]` déjà pleins (contrairement au Step 8 qui log).
Shift + 6 touches → Shift perdu. Cas limite.

### M8 — Macro à délai re-déclenchée à chaque changement matrice tant que tenue **[A, à vérifier intention]**
`main/input/key_processor.c:167, 344`. Presser une autre touche pendant la lecture
d'une macro (jouée en bloquant via `vTaskDelay`) re-arme `pending_macro_idx` →
rejeu. Auto-repeat voulu ou bug ? À confirmer.

### M9 — `tap_dance` : interruption n' pas résout la danse courante **[A]**
`main/input/tap_dance.c:95-96`. Commentaire « resolve current dance and reject »
mais le code fait juste `return false`. Divergence QMK. Comportemental.

### M10 — NVS `load_layout_names` sans garde de taille **[A]**
`main/input/keymap.c:99-120`. Même classe que E3, impact plus faible (cosmétique).

### M11 — Garde NVS = taille seule, pas de version/magic **[A]**
`main/input/keymap.c:162-170` · `main/sys/nvs_utils.c:36-44`. Un struct réordonné
**sans changer de taille** passe le garde → octets périmés réinterprétés
(macros/stats fausses silencieusement). Fix : préfixer un octet magic+version.

### M12 — `nvs_save_blob_with_total` ignore `nvs_set_u32` + `nvs_commit` **[A]**
`main/sys/nvs_utils.c:23-24`. Renvoie `ESP_OK` même si commit échoue → perte
silencieuse de stats ; un `NOT_ENOUGH_SPACE` au commit ne déclenche jamais
`bigram_save_disabled`. Fix : propager les deux erreurs.

### M13 — `MACRO_ADD` : clamp `nlen` après les checks de longueur **[A, bénin]**
`main/comm/cdc/cdc_binary_cmds.c:744-792`. Pas d'OOB (clamped ≤ original), mais si
le nom > 15, `keys`/`steps` lus à un offset décalé (données incohérentes). Fix :
clamper avant de calculer les offsets, ou rejeter `nlen >= MAX_MACRO_NAME_LENGTH`.

---

## ⚪ FAIBLE / cosmétique
- `key_stats[row][col]`/`key_stats_total` sans saturation (uint32, ~4,3e9 frappes —
  inatteignable). `key_stats.c:56-57`.
- `leader_init`/`leader_start` ne resettent pas `resolved_key/mod` (inoffensif,
  `resolved_flag` garde). `leader.c:91-117`.
- `ESP_LOGI` dans le hot-path RX USB avec `%s` sur données binaires. `usb_hid.c:312,317`.
- Labels dupliqués « Step 4 »/« Step 10 » dans `key_processor.c`.

## Connu / by-design (contexte, pas des bugs neufs)
- **RF non authentifié** → heartbeat sans anti-rejeu = keystroke injection
  (`heartbeat.c:27-47`) ; fix = auth de lien (HMAC), piste architecturale en
  attente de décision. Caveat MouseJack déjà documenté (`project_dongle_rf_security`).
- **Commandes destructrices CDC** (NVS_RESET 0xB1 / DFU 0x03 / OTA 0xF0/0xF1) non
  authentifiées = accès USB physique = contrôle total. By-design.

## Vérifié SAIN (pour ne pas re-signaler)
- **RF** : aucun OOB write/read over-the-air ; `MATRIX_STATE` doublement borné
  (decode 4-bit + `hb_apply_key` + `drain_radio`), buffer RX borné à la source
  (`rf_driver.c:571-583`), indices half/slot/trackpad sûrs. Fix H-3 tient.
- **CDC** : tous les index hôte bornés, toutes les lectures payload gardées,
  offsets OTA validés, `malloc` vérifié (hors hot-path). Aucun OOB.
- **key_stats/bigram** : indices bornés ; saturation `UINT16_MAX` correcte.
  `nvs_get_blob` borné par longueur → pas d'overflow depuis une taille inattendue.
- **key_processor** : injection mods bit→HID correcte, `caps_word` plages
  correctes, bornes `expand_macro`/row/col OK, LM borné.
- **leader** : fix préfixe complet, bornes saines (y compris buffer plein).

---

## Priorisation suggérée
1. **C1** (clamp couche LT/OSL) — sûreté mémoire, fix court, test facile.
2. **E1/E2** (tap_hold couche per-entry) — vrai bug utilisateur (couche bloquée).
3. **M1** (RF verify_rx one-liner) — masque une vraie panne radio.
4. **E3/E4** (NVS garde + faux OK) — perte de config utilisateur.
5. **E6** (key override) — feature cassée.
6. **E5** (CDC re-CRC/lock) — durcissement entrée hostile.
7. Le reste (MOYEN/edge) au fil de l'eau ; E7/RF-injection = décision archi (HMAC).

Tous les modules input ont maintenant des tests host branchés sur le vrai code
→ chaque fix peut se faire en TDD (test rouge → fix → vert), comme le bug leader.
