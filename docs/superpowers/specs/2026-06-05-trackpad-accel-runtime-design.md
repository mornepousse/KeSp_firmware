# Design — Trackpad : courbe d'accélération réglable à chaud (approche B)

Date : 2026-06-05
Statut : approuvé (brainstorming) → plan d'implémentation
But : rendre le pointeur trackpad agréable (précis en lent ET rapide en grand
déplacement) via une **courbe d'accélération**, réglable **à chaud depuis le
soft** (KeSp_controller) par le protocole CDC binaire.

## Problème

Le trackpad fonctionne mais le ressenti est mauvais : le mapping est un scaling
**linéaire 1:1** (`IQS5XX_SENS_NUM/DEN = 3/3`) appliqué **sur la moitié**, sans
courbe d'accélération. Impossible d'avoir à la fois la précision fine (lent) et
la traversée rapide de l'écran (rapide). Symptômes confirmés par l'utilisateur :
« trop lent / trop rapide » + « pas de précision fine ». Saturation et
lag/jitter NON retenus → hors scope.

## Décision d'architecture (approche B)

Aujourd'hui `rf_trackpad_t` transporte la **sortie traitée** (dx/dy int8) :
`trackpad_map()` tourne sur la **moitié**, le dongle ne fait que forwarder en
HID (`rf_rx_task.c` → `hid_send_mouse(tp.buttons, tp.dx, tp.dy, tp.scroll_v)`).

On **déplace le traitement sur le dongle** :
- la **moitié** devient un capteur « bête » : elle envoie le **brut**
  (`ge0, ge1, n_fingers, rel_x, rel_y`) dans le paquet NRF ;
- le **dongle** exécute `trackpad_map()` (gestes + courbe d'accel) avec une
  config en **NVS dongle**, réglée directement par CDC, puis émet le HID.

Pourquoi B (vs A = accel sur la moitié + config relayée par ESP-NOW) : le soft
parle au dongle en USB CDC ; mettre config + courbe + NVS sur le dongle évite de
bâtir le canal config ESP-NOW (réservé mais non implémenté), donne une
lecture/écriture instantanée, et passe la **pleine précision int16** dans la
courbe (pas de clamp int8 prématuré côté moitié). `trackpad_map()` prend déjà le
brut en entrée → c'est surtout un déplacement, pas une réécriture.

## Composants

### 1. Payload NRF `rf_trackpad_t` (changement de format)
- Avant : `{ i8 dx, dy; u8 buttons; i8 scroll_v, scroll_h; u8 seq }` (sortie).
- Après : `{ u8 ge0, ge1; u8 n_fingers; i16 rel_x, rel_y; u8 seq }` (brut).
- `rf_encode_trackpad` / `rf_decode_trackpad` (rf_packet.h) réécrits + tests host
  (`test_rf_packet.c`) mis à jour. `rel_x/rel_y` encodés big-endian sur le fil
  (cohérent avec le reste du protocole RF).
- ⚠️ Format de lien modifié → **moitié et dongle doivent être reflashés
  ensemble**. Bump de version firmware au moment de la release.

### 2. Courbe d'accélération (pure, host-testée) — dans `trackpad_map`
Nouveau paramètre `const trackpad_cfg_t *cfg`. Par report (virgule fixe, entier) :
- `vitesse = max(|rel_x|, |rel_y|)`
- `gain_x100 = base + (accel * vitesse) / ACCEL_DEN`, borné à `[base, gain_max]`
- `dx = clamp8(rel_x * gain_x100 / 100)`, idem `dy` (même gain sur les deux axes
  → diagonale cohérente).
- Scroll inchangé (`IQS5XX_SCROLL_DIV` constant ; pas réglable dans ce lot).
- `ACCEL_DEN` : constante de mise à l'échelle de la pente (ex. 100), fixée en
  dur ; seuls `base/accel/gain_max` sont réglables.

Propriétés visées : à faible vitesse `gain ≈ base` (< 1.0 possible → précision) ;
à forte vitesse `gain → gain_max` ; monotone croissant en vitesse.

### 3. Config `trackpad_cfg_t` + NVS dongle
```c
typedef struct {
    uint8_t  fmt;       /* = TRACKPAD_CFG_FMT (1), versioning */
    uint16_t base;      /* gain bas-régime ×100  (ex. 80  = 0.80×) */
    uint16_t accel;     /* pente : ×100 de gain ajouté par unité de vitesse / ACCEL_DEN */
    uint16_t gain_max;  /* plafond de gain ×100  (ex. 300 = 3.00×) */
} trackpad_cfg_t;       /* wire size packé = 7 octets LE */
```
- Défauts compilés (à tuner au banc), p.ex. `base=90, accel=40, gain_max=300`.
- Stockée en **NVS dongle** : namespace `"storage"`, clé `tp_cfg`, via
  `nvs_save_blob_with_total()`. Chargée au boot ; si absente/format inconnu →
  défauts. Validée (bornes) avant apply/save (cf. Gestion d'erreur).
- Encode/decode purs (`trackpad_cfg_encode/decode`, LE) — host-testés comme
  `ks_monitor`.

### 4. Commandes CDC (réglage à chaud) — binaire, KS/KR
- `KS_CMD_TRACKPAD_GET = 0xB8` : requête vide → réponse = `trackpad_cfg_t` packé
  (7 o). Idempotent.
- `KS_CMD_TRACKPAD_SET = 0xB9` : payload = `{base, accel, gain_max}` (6 o LE, sans
  `fmt`) → valide, applique en live (la config active du `trackpad_map` du
  dongle), sauve NVS, répond `OK` (status) + écho de la config appliquée.
- IDs libres (0xB7 = MONITOR ; 0xB8/0xB9 suivants). Documentées dans
  `docs/CDC_BINARY_PROTOCOL.md` (table offsets, bornes, sémantique) + exemples
  Python (`scripts/test_binary_protocol.py`) et C# (doc).

### 5. Refactor module trackpad / rôles (CMake)
- Scinder `main/periph/trackpad/` :
  - **pur** (compilé dongle + tests host) : `trackpad_map`, `trackpad_state_t`,
    `trackpad_cfg_t`, encode/decode cfg, courbe d'accel. Ne dépend que de
    `<stdint.h>` + `rf_packet.h`.
  - **matériel half** (`CONFIG_KASE_HAS_TRACKPAD`) : I2C, RST, RDY ISR, lecture
    du bloc 9 octets, extraction du brut, forward NRF. N'appelle plus
    `trackpad_map`.
- **Dongle** (`CONFIG_KASE_HAS_RF_RX`) : appelle `trackpad_map(..., &cfg)` à la
  réception d'un paquet trackpad, puis `hid_send_mouse`. Charge/applique
  `trackpad_cfg` (NVS) ; héberge les handlers CDC 0xB8/0xB9.
- Mettre à jour `main/CMakeLists.txt` (le fichier pur compilé côté dongle ; la
  partie I2C reste sous `CONFIG_KASE_HAS_TRACKPAD`) et `test/CMakeLists.txt`.

## Gestion d'erreur
- CDC SET : valider les bornes (`base ≤ gain_max`, `gain_max ≤` plafond dur ex.
  1000 = 10×, `accel ≤` borne). Hors bornes → répondre `KS_STATUS_*` d'erreur,
  ne rien sauver. Jamais de crash.
- NVS absente/corrompue/format `fmt` inconnu au boot → défauts compilés, log,
  pas d'erase.
- Paquet NRF trackpad de longueur inattendue → `rf_decode_trackpad` renvoie
  false, paquet ignoré (déjà le pattern actuel).

## Tests
- **Host** (`test/`) :
  - `trackpad_map` avec cfg : petit `rel` → `gain ≈ base` ; grand `rel` →
    `gain` plafonné à `gain_max` ; monotonie ; clamp8 ; **les gestes existants
    (tap 1/2/3 doigts, scroll, drag) restent identiques** (tests actuels verts).
  - `rf_encode/decode_trackpad` nouveau format brut (round-trip, big-endian
    rel_x/rel_y, longueurs).
  - `trackpad_cfg_encode/decode` (offsets, LE, fmt, round-trip).
- **Hardware** : feel sur la moitié à trackpad ; réglage live via 0xB9 puis
  vérification du nouveau comportement sans reflash ; persistance après reboot
  (relire via 0xB8).

## Découpage (pour writing-plans)
1. **Refactor flux B** : payload brut + `trackpad_map` déplacé sur le dongle,
   gain linéaire par défaut (= comportement identique). Valide la plomberie
   end-to-end sans changer le feel. (firmware + tests payload)
2. **Courbe d'accel** dans `trackpad_map` (+ param cfg avec défauts compilés) +
   tests host. Le vrai gain de confort.
3. **Réglage à chaud** : `trackpad_cfg` NVS dongle + CDC 0xB8/0xB9 + doc +
   exemples Python/C#.

Chaque lot produit un firmware fonctionnel et testable.

## Critères de succès
- Pointeur précis en lent ET rapide en grand déplacement (courbe d'accel).
- Réglage `base/accel/gain_max` à chaud depuis le soft via 0xB9, effet immédiat,
  persistant après reboot (0xB8 relit la valeur).
- `trackpad_map` (courbe) + encode/decode payload + cfg couverts par tests host ;
  gestes existants non régressés.
- Les 6 boards compilent (`scripts/check.sh`).

## Hors scope (YAGNI)
- Scroll réglable / accel de scroll (constante pour l'instant).
- Réglage de la résolution/report-rate de la puce IQS5xx (réglages par défaut).
- Anti-jitter / deadzone, anti-saturation (symptômes non retenus).
- UI soft (KeSp_controller) : on fournit protocole + exemples, l'UI est dans
  l'autre repo.
- Profils multiples / par-application.
