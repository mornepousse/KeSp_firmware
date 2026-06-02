# Design — Snapshot de monitoring CDC (KS_CMD_MONITOR)

Date : 2026-06-02
Statut : approuvé (brainstorming) → plan d'implémentation
But : permettre au soft (KeSp_controller) d'afficher un gros panneau de
monitoring en pollant UNE commande CDC consolidée.

## Problème / objectif

Le protocole CDC binaire expose déjà beaucoup de télémétrie, mais éparpillée
sur ~6-8 commandes (`RF_STATUS`, `BATTERY`, `WPM_QUERY`, `BT_QUERY`,
`LAYER_INDEX`, …). Un dashboard qui rafraîchit à 1-2 Hz devrait faire 6-8
allers-retours par refresh. On ajoute **une commande snapshot consolidée** qui
renvoie tout le "live" en un seul aller-retour, + la santé système qui manque
aujourd'hui (uptime/heap/temp).

Décisions (brainstorming) :
- **Snapshot consolidé** (pas poll-par-métrique, pas streaming push).
- Couvre : RF live (signal/link/USB), batterie par moitié, santé système, état
  clavier (layer/BT/WPM/keys_total).
- Les **gros payloads** (heatmap `KEYSTATS_BIN`, `BIGRAMS_BIN`) restent des
  commandes séparées (onglet dédié, pollés moins souvent) — hors snapshot.
- Marche sur le **dongle** (RF complet) ET sur un **clavier autonome** (V1/V2/V2D :
  `has_rf=0`, champs RF/batterie à 0).
- Format **versionné** (`fmt` en tête) pour évoluer sans casser le soft.

## Commande

`KS_CMD_MONITOR = 0xB7` (libre ; 0xB0-0xB6 pris).
Requête : `KS [0xB7] [len=0] [crc]` (pas de payload).
Réponse : `KR [0xB7] [status=OK] [len][ks_monitor_t packé][crc]`.
Idempotent, sans effet de bord. Cadence soft recommandée : 1-2 Hz.

## Payload `ks_monitor_t` (packé, little-endian)

| Off | Champ | Type | Notes |
|----|----|----|----|
| 0 | fmt | u8 | = 0x01, version du format |
| 1 | flags | u8 | bit0=has_rf, bit1=link_L, bit2=link_R, bit3=usb_active, bit4=bt_connected |
| 2 | uptime_s | u32 LE | `esp_timer_get_time()/1e6` |
| 6 | heap_free_kb | u16 LE | `esp_get_free_heap_size()/1024` (clamp 0xFFFF) |
| 8 | temp_c | i8 | capteur si dispo, sinon `INT8_MIN` (-128 = inconnu) |
| 9 | layer_idx | u8 | `current_layout` |
| 10 | wpm | u8 | WPM courant (clamp 255) |
| 11 | keys_total | u32 LE | total frappes (key_stats) |
| 15 | sig_left | u8 | 0..255 (0 si !has_rf) |
| 16 | sig_right | u8 | 0..255 |
| 17 | hb_age_L_ms | u16 LE | âge dernier heartbeat moitié gauche (clamp 0xFFFF) |
| 19 | hb_age_R_ms | u16 LE | |
| 21 | batt_L_dV | u8 | tension ×10 (0=inconnu) |
| 22 | batt_L_soc | u8 | 0..100 |
| 23 | batt_L_chg | u8 | 0/1 |
| 24 | batt_R_dV | u8 | |
| 25 | batt_R_soc | u8 | |
| 26 | batt_R_chg | u8 | |
| 27 | bt_slot | u8 | slot BT actif |

Taille = **28 octets**. Sur clavier autonome : `has_rf=0`, offsets 15..26 = 0.

## Sources (toutes existantes)
- uptime : `esp_timer_get_time()`. heap : `esp_get_free_heap_size()`.
- temp : capteur temp si initialisé (cf. eink_lvgl utilise `temperature_sensor_get_celsius`) ; sinon `INT8_MIN`.
- layer : `current_layout`. wpm : getter WPM (KS_CMD_WPM_QUERY). keys_total : key_stats.
- RF (dongle) : `rf_rx_get_status()` (link L/R, sig, hb_age). usb : `tud_ready()`.
- batterie (dongle) : cache dongle (source de `KS_CMD_BATTERY`). `half_batt_soc_pct()` calcule soc depuis dV si besoin.
- BT : `bt_query` (slot, connected).
- has_rf : tag `RF_DONGLE` (FEATURES) / `CONFIG_KASE_HAS_RF_RX`.

## Découpage : gather (hardware) vs encode (pur)
- **`ks_monitor_encode(uint8_t *buf, const ks_monitor_t *m)`** — packing PUR
  (offsets, LE). Testé host (norme TDD).
- **`bin_cmd_monitor()`** — handler : remplit un `ks_monitor_t` depuis les
  sources hardware, appelle `ks_monitor_encode`, répond via `ks_respond`. Non
  testé host (hardware).

## Compat soft
- Doc dans `docs/CDC_BINARY_PROTOCOL.md` : commande 0xB7 + table des offsets + sémantique des flags + note has_rf.
- Exemple client : ajout dans `scripts/test_binary_protocol.py` (poll 0xB7, parse, print) + un snippet C# (struct + parsing) pour le soft WPF dans la doc.
- `FEATURES` (0x02) inchangée ; le soft détecte `RF_DONGLE` pour savoir s'il y a des moitiés.

## Gestion d'erreur
- Si une source est indispo (pas de capteur temp, pas de RF) → valeur sentinelle
  (temp INT8_MIN, RF/batt = 0, flag has_rf=0). Jamais d'échec : la commande
  répond toujours OK avec ce qui est connu.
- Le `fmt` permet au soft de rejeter/adapter si version inconnue.

## Tests
- **Host (TDD, `test/test_cdc_monitor.c`)** : `ks_monitor_encode` — vérifier
  chaque offset, l'endianness LE (uptime_s, heap, keys_total, hb_age), les flags
  (has_rf etc.), la sentinelle temp, le clamp heap/wpm. Ajouté à
  `test/CMakeLists.txt` + `test/test_main.c`.
- **Hardware** : poll 0xB7 via `scripts/test_binary_protocol.py` sur le dongle
  (has_rf=1, champs RF remplis) et sur un V2 (has_rf=0).

## Critères de succès
- Le soft poll 0xB7 à 1-2 Hz et reçoit tout le live en 1 frame.
- Champs corrects dongle (RF/batterie) et autonome (has_rf=0).
- `ks_monitor_encode` couvert par tests host (offsets/endianness/flags).
- Doc + exemple client permettent au soft de parser sans deviner.

## Hors scope (YAGNI)
- Streaming/push (écarté). Heatmap keystats + bigrams (commandes séparées
  existantes). Historique/persistance (le soft garde son propre historique).
- Contrôle/écriture via le snapshot (lecture seule ; les actions restent leurs
  commandes dédiées).
