# Fusion au dongle et deux moteurs keymap — design

Rédigé le 2026-09-12, après le constat d'autonomie de la moitié gauche.
Statut : **proposition**, à discuter avant toute implémentation. Révise en partie
`2026-08-19-dongle-role-niphargus-design.md` (le dongle n'est plus un pur relais).

## Le problème

L'architecture actuelle fait de la **gauche le maître unique** : elle porte le
seul moteur keymap, fusionne sa matrice avec celle de la droite reçue par radio,
et envoie du **HID fini** au dongle (`kbd_relay_send_kbd`, `hid_report.c:116/192/240` ;
le dongle relaie `PKT_TYPE_HIDREPORT`, `rf_rx_task.c:171`). Le dongle est bête, il
n'y a qu'un moteur — c'était le choix explicite de `2026-08-19-dongle-role`.

Conséquence sur l'énergie, mesurée en pratique : **la gauche doit écouter la
droite en continu** (PRX, ~13 mA) tant qu'elle est maître et éveillée. Émettre
est gratuit, écouter est cher. La droite, émettrice pure, peut dormir comme un
clavier sans fil normal ; la gauche non. C'est le plafond d'autonomie du côté
gauche, et ça crée aussi l'asymétrie de réveil (une gauche endormie est sourde à
la droite → « touche la gauche d'abord », latence au réveil).

## La décision

**Les deux moitiés émettent leur matrice BRUTE vers le dongle. Le moteur keymap
tourne à l'endroit ACTIF, et il n'y en a qu'un actif à la fois, choisi par le
routage.**

- **Sans fil** : gauche et droite émettent leur demi-matrice brute au dongle ;
  aucune n'écoute ; le **dongle** fusionne, fait tourner le moteur, sort le HID
  fini vers l'hôte USB.
- **USB branché sur la gauche** : la gauche est sur secteur (autonomie hors
  sujet), elle peut donc se permettre d'écouter la droite ; **son** moteur
  tourne et sort le HID par son propre USB. Le dongle se tait.

Aucune moitié n'écoute quand elle est sur batterie → les deux atteignent
l'autonomie d'un clavier sans fil simple. C'est le gain visé.

## Deux moteurs sans divergence — les trois règles

Deux moteurs ne divergent que si leur **config** diffère ou si leur **état
d'exécution** se chevauche. On neutralise les deux :

### 1. Même code
`key_processor` / `build_keycode_report` et toute la logique de couches, combos,
tap-hold sont déjà un module partagé, compilé dans les deux firmwares. À config
et état égaux, comportement identique par construction. Contrainte : ne jamais
laisser les deux firmwares dériver en version (voir règle 2, le verrou de
version sert aussi à ça).

### 2. Une seule source de config, répliquée et versionnée
La keymap, les couches, macros, réglages — une seule vérité, écrite par le
contrôleur, **répliquée dans les deux NVS** (gauche ET dongle). C'est le coût
nouveau principal :

- Le contrôleur écrit les deux au remappage.
- Une **empreinte de config** (CRC-32 du blob keymap) sert de version — affiné
  par rapport au « numéro de version » initial : une empreinte de CONTENU se met
  à jour d'elle-même et détecte toute divergence, sans numéro à tenir à la main.
  Logique pure `config_fp_crc32` (`main/input/config_sync.h`), testée host ;
  exposée par CDC `KS_CMD_CONFIG_FINGERPRINT` (0x16) sur la gauche ET le dongle.
  Le contrôleur lit les deux : égales = synchronisées. Le moteur du dongle
  refusera de tourner (repli / erreur) si l'empreinte annoncée par la gauche ≠
  la sienne, plutôt que de produire un comportement différent en silence — ce
  garde-fou runtime suppose l'annonce par RF (à venir).
- Le format du blob reste celui de `KEYMAP_BLOB_BYTES` (`keymap.h`), dimensionné
  sur `KEYMAP_COLS` — donc les deux endroits stockent les 14 colonnes complètes.

Sans cette règle, brancher ou débrancher changerait le comportement : c'est LA
divergence à éviter.

### 3. Un seul moteur actif à la fois, par le routage
L'état d'exécution (couche MO tenue, one-shot, caps word, tap-hold en cours,
séquence leader) ne vit qu'à **un** endroit à la fois. Le routage tranche :
USB présent sur la gauche → moteur gauche ; sinon → moteur dongle. Jamais les
deux → jamais deux états concurrents. La bascule réutilise le mécanisme
USB-first existant (`usb_presence` / `kbd_active_route`).

## Le changement de transport

Aujourd'hui la gauche envoie du HID fini. Le nouveau transport envoie de la
**matrice brute** des deux moitiés, et le moteur actif fusionne :

- Réutiliser le format demi-matrice existant (`rf_packet.h`, bitmap 4×7 déjà
  employé pour le lien droite→gauche). La gauche, en mode sans fil, émet sa
  propre demi-matrice au dongle au lieu de fusionner localement.
- Le dongle reçoit deux demi-matrices (slot clavier gauche + un nouveau flux
  droite), les fusionne selon la même règle de miroir que le maître
  (`half_col_to_keymap`, `BOARD_REMOTE_COLS_MIRRORED`), et fait tourner le moteur.
- Le **trackpad** suit la même logique : la gauche envoie ses gestes bruts
  (`tp_frame_t`), le moteur actif applique `trackpad_map`. C'est l'« approche B »
  qui existait déjà côté dongle et qui avait été retirée — elle revient, mais
  cette fois cohérente avec le reste (le moteur actif mappe tout).

## Ce qui est touché dans le code

- **rf_rx_task.c** : le dongle gagne un moteur. `drain_radio` cesse de relayer
  `PKT_TYPE_HIDREPORT` tel quel ; il alimente une fusion + `build_keycode_report`
  + sortie HID locale. Deux flux de matrice brute (gauche, droite) au lieu d'un
  flux HID.
- **kbd_relay_tx.c / hid_report.c** : en mode sans fil, la gauche émet sa
  demi-matrice brute, pas `kbd_relay_send_kbd(HID fini)`. Le chemin USB-direct
  de la gauche garde son moteur local.
- **half_link.c** : le lien droite→gauche disparaît au profit de droite→dongle ;
  ou la droite émet vers les deux (voir questions ouvertes).
- **Config NVS** : réplication + versionnage, côté contrôleur et firmware.
- **Le dongle** doit compiler le moteur keymap complet — il ne le fait pas
  aujourd'hui.

## Risques et coûts

- **Le dongle grossit** : moteur, fusion, config NVS, trackpad. Plus un pur relais.
- **Sync de config à deux NVS** : le vrai point dur. Un remappage qui ne met à
  jour qu'un côté casse la cohérence. Le versionnage la détecte mais ne la
  corrige pas seul.
- **On inverse un choix acté** (`2026-08-19-dongle-role`). Ce document doit être
  révisé, pas juste contredit.
- **Le mode USB-autonome de la gauche est préservé** mais devient un chemin
  distinct à tester à part entière (son moteur + son écoute de la droite).

## Décisions arrêtées

- **Question 1 — routage de la droite : TRANCHÉ le 2026-09-12.** La droite émet
  **toujours vers le dongle**, jamais vers la gauche. En mode USB-gauche, c'est
  le **dongle qui réémet** la demi-matrice de la droite vers la gauche. La droite
  reste ainsi bête et ignorante du mode : un seul émetteur, une seule cible, une
  seule adresse — pas d'état qu'elle ne peut pas voir (l'USB est branché à
  l'autre bout). Conséquence : le lien direct droite→gauche (`half_link`,
  KaSe.03) disparaît au profit de droite→dongle ; c'est le dongle qui gagne le
  saut supplémentaire vers la gauche quand le moteur tourne à gauche. L'auto-ACK
  ESB point à point reste satisfait à chaque bond (droite↔dongle, puis
  dongle↔gauche), aucun récepteur muet.

## Questions ouvertes
2. **Réveil sans fil** : deux émetteurs purs qui dorment ne s'entendent pas plus
   qu'avant. Le dongle (sur secteur) entend les deux → chaque moitié réveille le
   système via le dongle. Le « touche la gauche d'abord » disparaît. À confirmer.
3. **Sync de config** : push simultané depuis le contrôleur, ou le dongle tire
   la config de la gauche à l'appairage ? La seconde évite deux écritures mais
   ajoute un protocole.
4. **Le trackpad** repasse en gestes bruts : réintroduire le parseur côté dongle
   (il existe encore, `periph/trackpad/`), retirer le mapping du maître.

## Décision attendue

Avant de coder : valider les trois règles, choisir la réponse à la question 1
(diffusion vs réémission), et acter la révision de `2026-08-19-dongle-role`.
