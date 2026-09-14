# Jauge batterie des moitiés Niphargus — design

Date : 2026-09-14
Statut : design validé, spec à relire avant plan d'implémentation.

## Contexte et problème

Chaque moitié du Niphargus tourne sur une Li-ion 16340 (DW01A + FS8205 →
HT7833 ; charge TP4056 ~500 mA, LED CHRG, **STDBY non câblé**). Le firmware
prévoit déjà tout le chemin de remontée d'une tension — champ `batt_dV` de
`PKT_TYPE_STATUS`, cache batterie du dongle, commande CDC `KS_CMD_BATTERY`
(slot 0 = gauche, 1 = droite : dV, soc, charging, age) — mais **personne ne
mesure** : `batt_dV` vaut 0 partout, le dongle répond « inconnu », et aucune
lecture ADC n'existe dans le firmware.

Trois usages demandés (v1 et v2) : remontée au dongle et au contrôleur PC ;
détection de fin de charge (la doc matérielle dit « fin de charge par ADC »,
faute de STDBY) ; comportement batterie faible.

## Matériel (docs/NIPHARGUS_V2_HARDWARE.md, vérifié)

- `VBAT_SENSE` = **GPIO13 = ADC2_CH2**, pont **1 MΩ / 1 MΩ + 100 nF** → la
  batterie pleine (≈ 4,15 V) donne ≈ 2,08 V à l'ADC. Identique sur les deux
  moitiés (`BOARD_VBAT_SENSE_GPIO` défini des deux côtés).
- ADC2 est utilisable : pas de WiFi (nRF24 seul).
- Load-sharing AO3407 : USB présent ⇒ batterie isolée de la charge système,
  mais `VBAT_SENSE` reste sur la borne batterie (on lit la tension de charge).
- **Pas de détection VBUS** : le pont GPIO33 n'est pas peuplé
  (`CONFIG_KASE_VBUS_SENSE` sans matériel) ; `tud_mounted()` ne voit qu'un
  hôte, jamais un chargeur mural. « En charge » n'est donc PAS mesurable
  directement — voir §5.

## Décisions actées

1. **Batterie avant écrans** (l'écran affichera la jauge).
2. **Remontée par STATUS** avec un drapeau d'identité de moitié ; la droite,
   muette au repos, émet un STATUS **lent**. L'utilisateur : « il n'y a pas de
   changements rapides, une remontée lente me va ».
3. v1 = mesure + remontée par moitié + plateau « pleine » ; v2 = seuils faibles
   et leurs effets (spécifiés ici, livrés ensuite).

## 1. Mesure (module `power/batt_sense`, chaque moitié)

- `adc_oneshot` sur ADC2_CH2, atténuation 12 dB (0 – ~3,1 V, 2,1 V dedans),
  calibration `adc_cali` (courbe S3) quand disponible, sinon lecture brute
  documentée comme telle.
- Une mesure = **8 échantillons moyennés** après un délai de stabilisation
  (le pont à 1 MΩ est haute impédance ; le 100 nF tient l'échantillonnage).
- Conversion pure : `mV_adc × 2 → mV_batt → dV` (`uint8`, 42 = 4,2 V, l'unité
  déjà utilisée par `batt_dV`). Rejet : `< 2,5 V` ou `> 4,5 V` ⇒ **inconnu**
  (capteur absent, pont ouvert, erreur ADC). Valeur « inconnu » = 0, déjà la
  convention du protocole (0 = inconnu).
- Cadence : une mesure toutes les **10 s** éveillée, une au **réveil** ; jamais
  pendant le light sleep. Coût négligeable (quelques µs d'ADC toutes les 10 s).
- Logique pure, testée host : conversion + rejet + moyenne, seuils (§4-5).

## 2. Remontée radio

- `PKT_TYPE_STATUS` gagne `PKT_STATUS_FLAG_HALF_RIGHT = 0x2` dans le nibble
  bas de l'octet 0 (`0x1` = mode_usb déjà pris). Bit à 0 ⇒ gauche :
  **rétrocompatible**, toute trame ancienne reste « gauche ». `rf_status_t`
  gagne `uint8_t half`.
- **Gauche** : inchangée en cadence (STATUS 1/s en sans-fil, `RF_STATUS_PERIOD_MS`),
  `batt_dV` = dernière mesure valide.
- **Droite** : un STATUS toutes les **`RF_BATT_PERIOD_MS` = 30 000 ms** au repos,
  plus un au réveil, émis par sa tâche de rafraîchissement vers sa cible
  courante. Si elle est repliée sur la gauche (dongle absent), la gauche ignore
  une trame STATUS — acceptable, la batterie de la droite est alors
  simplement « inconnue » côté dongle absent. `RF_BATT_PERIOD_MS` vit dans
  `rf_slot.h` : c'est un contrat entre firmwares, pas un nombre local.
- Le STATUS de la droite ne compte PAS comme activité clavier (pas de tampon
  de veille) : il ne doit pas empêcher la droite de dormir.

## 3. Dongle et CDC

- Le cache batterie est indexé par **moitié** (`half` lu dans le drapeau), plus
  par slot : les deux moitiés partagent le slot clavier en fusion.
- `KS_CMD_BATTERY` **ne change pas** (slot 0 = gauche, 1 = droite ; dV, soc,
  charging, age_ms). Le contrôleur n'a rien à modifier ; `age_ms` porte la
  fraîcheur (≈ 30 s côté droite = normal, la doc CDC tolère déjà ~30 s).
- Hors fusion (dongle pré-fusion), rien ne change : STATUS gauche ⇒ half=0.

## 4. Pleine / en charge (v1)

« En charge » n'étant pas mesurable, on déduit :
- `charging = inconnu (0xFF)` par défaut ;
- **PLEINE** : plateau **≥ 4,15 V pendant ≥ 2 min** (TP4056 en fin de charge
  tient 4,2 V et réduit le courant ; STDBY absent, c'est la « fin de charge
  par ADC » de la doc) ;
- **EN_CHARGE_PROBABLE** : tension qui **monte** de ≥ 0,1 V en < 5 min (une
  décharge ne monte jamais).
Machine à états pure (`batt_state_step(dV, now)`), hystérésis 0,05 V, testée.
Exposée dans `charging` (0 = non/inconnu, 1 = en charge probable, 2 = pleine).

## 5. Batterie faible (v2 — spécifié, livré après v1)

Seuils Li-ion 16340 : **avertissement ≤ 3,5 V**, **critique ≤ 3,3 V**, avec
hystérésis 0,1 V (le DW01A coupe vers 2,5-3,0 V ; on prévient bien avant).
`soc_pct` approximé par une table tension→% (4,15 V = 100, 3,9 = 70, 3,7 = 40,
3,5 = 15, 3,3 = 0), pure et testée. Effets (écran, LED, refus d'actions) :
hors périmètre de cette spec — ils viendront avec l'écran.

## Unités

- Pur : `batt_mv_to_dv`, moyenne/rejet, `batt_state_step`, table SoC,
  encode/decode STATUS avec `half` (test_rf_packet étendu).
- Cible : `power/batt_sense.c` (ADC), câblage dans `kbd_relay` (gauche) et
  `half_link` (droite, STATUS lent), `rf_rx_task`/`dongle_state` (cache par
  moitié). Aucun changement CDC.

## Vérification

- Host : tests purs rouges avant impl, mordants à la mutation.
- Boards : gauche/droite fusion + défaut, dongle fusion + défaut, `kase_v2`.
- Banc : `KS_CMD_BATTERY` affiche deux tensions plausibles (≈ 3,7–4,2 V) avec
  `age` frais ; débrancher/rebrancher la charge fait apparaître PLEINE après
  le plateau ; une moitié éteinte → « inconnu » après le timeout.
- Contrat : lignes `COMPORTEMENTS.md` (pures) + `[smoke:Jauge batterie]`.

## Risques

- ADC2 + calibration : les courbes S3 sont fournies par ESP-IDF ; à défaut,
  lecture brute et erreur documentée (±0,1 V acceptable pour une jauge).
- Haute impédance du pont : si la mesure est bruitée, augmenter la moyenne ou
  le délai — jamais baisser le rejet.
- « En charge » restera une inférence tant que le pont VBUS n'est pas peuplé :
  documenté, exposé comme tel au contrôleur.

## Hors périmètre

Effets de batterie faible sur l'UI, écran, estimation d'autonomie restante,
compensation de température.
