# Écrans Sharp memory-LCD des moitiés Niphargus — design

Date : 2026-09-14
Statut : livré le 2026-09-14 (Tasks 1-6) ; **§4 retiré** le même jour sur
décision utilisateur (« connaître la tension de l'autre, on s'en fout »).

## Contexte et problème

Les deux moitiés portent désormais un module type nice!view — **Sharp
LS011B7DH03, 160 × 68, 1 bit, SPI write-only**, monté **en portrait (68 de
large × 160 de haut)** — soudé sur J4 (droite) et J12 (gauche). Le firmware n'a
aucun pilote : `BOARD_LCD_CS_GPIO` (GPIO14, **actif haut**) n'est jamais lu, le
CS flotte au boot, et `boards/niphar_left/board.h` déclare encore J12 vide.

Le bus SPI est **partagé avec le nRF24** (SCK 38 / MOSI 40 ; l'écran n'a pas de
MISO). CLAUDE.md documente trois pannes silencieuses dans cette zone (« une puce,
un propriétaire », bascule de mode `spi_parquer_mode`) : le pilote écran doit
s'y plier, pas les redécouvrir.

## Décisions actées avec l'utilisateur

- **Contenu v1** : couche active (**statique** : TO / couche de base — pas les
  MO/LT tenus), jauge batterie (la sienne + celle de l'autre moitié), état du
  lien (route RF/USB, dongle vu), splash au boot.
- **Deux écrans, même backend** : même bandeau et même pied des deux côtés ;
  seule la zone centrale diffère — **gauche : la couche**, **droite : le logo**.
- **Énergie** : l'écran ne vit qu'éveillé ; en light sleep l'image est **gelée**
  (le memory-LCD la conserve sans horloge), aucun réveil pour l'écran.
- **Mise en page (maquettes validées, portrait 68 × 160)** :
  - bandeau haut : route (`RF`/`USB`) + `▲` dongle vu · **sa** batterie en jauge
    verticale + tension (UNSCII 8) ;
  - centre gauche : nom de la couche statique en Montserrat 14, **coupé en
    lignes de 3-4 caractères** (choix « option 1 » : lisible sans tourner la
    tête) ; règle : 4 caractères par ligne, 3 lignes max, puis `…` ; index de
    couche (`L1`) dessous ;
  - centre droite : **le vrai logo** `Niphargus/images/niphargus_logo.svg`
    rendu en 1 bit à **60 px de large, centré** horizontalement (4 px de marge)
    et verticalement entre bandeau et pied ;
  - pied : batterie de **l'autre** moitié (`DROITE 4.2V ■` / `GAUCHE 4.0V`),
    `■` = pleine, `?` = inconnue.
- **Données descendantes vers les moitiés : approche A** — une petite trame
  `PKT_TYPE_DISPLAY` que le dongle glisse dans l'**ACK payload** de chaque trame
  reçue d'une moitié (canal déjà prouvé par la sync keymap). Rejetées : B (la
  moitié qui tape sait tout — faux en fusion), C (fenêtre d'écoute — coûte
  l'autonomie).

## 1. Pilote panneau (`display/memlcd/memlcd_panel.c`)

Protocole Sharp memory-LCD (public, à **prouver au banc** — la datasheet du
LS011B7DH03 n'est pas dans la bibliothèque lemia) :
- SPI mode 0, **≤ 1 MHz** (2 MHz max spec). L'ESP32 émet MSB-first et le
  panneau lit le PREMIER bit clocké comme M0 (app note Sharp, lemia doc 6845
  p. 10-11) : le mot de commande part **brut** (`0x80` = write, `0x40` = VCOM,
  `0x20` = clear) ; seule l'**adresse de ligne** se lit CA0 en premier et passe
  par `memlcd_rev8` ; les pixels partent D1 en premier (bit 7), D = L → noir.
- **CS actif HAUT** : `BOARD_LCD_CS_ACTIVE_HIGH`. Piloté **bas dès le boot des
  deux côtés** (même si l'écran est absent) pour qu'il n'écoute jamais le trafic
  nRF sur le bus partagé.
- **Géométrie physique : 68 lignes × 160 px** (catalogue Sharp, lemia doc 6844
  p. 5 : « 160 × 68 », H = sens des données). Le portrait 68 × 160 de l'UI est
  une rotation de 90° : `memlcd_fb_to_panel` (pure, testée) transpose le tampon
  portrait en 68 lignes de 20 octets ; `BOARD_LCD_ROTATE_180` retourne l'image.
- Trame « write line » : `[0x80|M1][rev8(adresse 1..68)][160 px = 20 o][0x00]`,
  plusieurs lignes à la suite, `0x00` final. Une image entière = 68 × 22 o
  ≈ 1,5 Ko, ~12 ms à 1 MHz ; on réécrit l'image quand le modèle change.
- **VCOM** : bit M1 basculé à chaque écriture, et une trame « VCOM seul »
  (`[M1][0x00]`) **~1 Hz** quand rien ne change, tant que la carte est éveillée.
  En light sleep : rien (image conservée ; le panneau tolère l'absence de VCOM
  pendant les heures d'une veille — c'est le compromis énergie acté).
- Effacement : `[M2=1][0x00]`.
- **Bus partagé** : toute transaction écran se fait **sous le verrou radio** de
  la moitié (`s_tx_radio_mux` côté droite / `s_tx_mutex` côté gauche, via un
  petit accesseur `rf_bus_lock()/unlock()` exposé par le propriétaire) — jamais
  pendant un envoi nRF. L'écran est en mode 0 comme le nRF : `spi_parquer_mode`
  n'entre pas en jeu, mais le device SPI de l'écran est ajouté sur le même hôte
  avec `spics_io_num = -1` (CS manuel, comme la radio) et `max_transfer_sz`
  (5120) couvre une image entière.
- Orientation : LVGL travaille en **68 × 160** ; le panneau physique est 160
  lignes de 68 px utiles — en portrait natif, ligne LVGL = ligne panneau (pas de
  rotation logicielle si le module est monté connecteur en bas ; sinon rotation
  180° par simple inversion d'ordre, réglable par `BOARD_LCD_ROTATE_180`).

## 2. Backend LVGL (`display/memlcd/memlcd_backend.c`)

Implémente `display_backend_t` (init/update/update_layer/refresh_all/sleep/
wake/notify_*/show_dfu) à côté des backends OLED et rond ; sélectionné par
`CONFIG_KASE_DISPLAY_MEMLCD` (nouveau), `KASE_HAS_DISPLAY` passant à `y` pour
les deux moitiés. LVGL 8 en **1 bit** (`LV_COLOR_DEPTH` reste 16 dans le
projet : le flush convertit en 1 bit par seuil, comme le backend OLED le fait
pour le SSD1306) ; le `flush_cb` transmet au pilote les lignes touchées. Cadence
`update()` : 100 ms (ne redessine que si une donnée a changé) ; VCOM à 1 Hz
depuis le même timer. `sleep()` = arrêt du timer, image gelée ; `wake()` =
reprise + redessin complet.

Assets : le logo est une **image LVGL 1 bit générée** par
`scripts/gen_logo_memlcd.sh` (Inkscape → PNG 60 px → seuil 128 →
`main/display/assets/img_niphargus_60.c`, `LV_IMG_CF_ALPHA_1BIT`), commité avec
sa commande de génération — reproductible, jamais tapé à la main.

## 3. Modèle d'affichage (`display/memlcd/memlcd_model.h`, pur)

Un `memlcd_model_t` : `route`, `dongle_vu`, `batt_local_dv/chg`,
`couche`, `nom[16]`, `is_left`. Deux
fonctions pures testées :
- `memlcd_couper_nom(const char *nom, char lignes[3][5])` — 4 caractères par
  ligne, 3 lignes max, `…` si tronqué, jamais de ligne vide au milieu ;
- `memlcd_model_diff(a, b)` — true si un champ **affiché** a changé (pilote le
  « ne redessine que si nécessaire »).

## 4. Données descendantes : RETIRÉ (2026-09-14)

Une trame `PKT_TYPE_DISPLAY` dans l'ACK payload a été implémentée et prouvée
au banc sur les deux moitiés, puis **retirée le jour même** : l'utilisateur ne
veut pas de la batterie de l'autre moitié à l'écran, et la couche statique
n'est affichée que sur la gauche, qui la connaît localement. Sans consommateur,
le canal disparaît (pas de code mort dans une trame radio) et l'ACK reste nu
hors sync keymap. Ce que l'exercice a appris, gardé dans `rf_rx_task.c` :
- les deux moitiés partagent le pipe 0 du dongle, donc une charge d'ACK part
  avec le **prochain** ACK quel qu'en soit l'émetteur — une trame « pour la
  droite » se fait manger par la gauche en USB (5 annonces/s) ;
- une excursion `rf_driver_oob_tx` (réémission droite→gauche) vide la FIFO TX
  du PRX : la charge d'ACK se charge désormais **en fin de tour**, après
  l'excursion (`drain_radio`), ce qui vaut aussi pour la sync keymap.
`rf_driver_oob_tx_ap` (excursion qui rend la charge de l'ACK) reste dans le
pilote, sans appelant : c'est le seul canal descendant vers une moitié en PRX
sur un autre canal, si un besoin revient.

## Périmètre v1, dans l'ordre de preuve

1. Bring-up : CS bas au boot, effacement, **un motif de test** (damier) — prouver
   le protocole sur la droite (FTDI dessus), puis la gauche.
2. Splash + bandeau + pied avec les données **locales** (route, sa batterie).
3. Gauche : la couche statique (locale en USB ; via DISPLAY en RF).
4. Droite : le logo centré.
5. DISPLAY dans l'ACK : couche + batterie de l'autre moitié des deux côtés.
6. Veille : gel/reprise, aucun réveil dû à l'écran.

## Vérification

- Host : `memlcd_couper_nom`, `memlcd_model_diff`, encode/decode DISPLAY,
  inversion de bits LSB-first — tests purs rouges avant impl, mordants.
- Cible : builds gauche/droite fusion + défaut (écran activé) ; les 7 boards par
  défaut restent verts ; le dongle fusion + défaut.
- Banc : damier net sans pixels parasites (sinon LSB/ordre de lignes) ; splash ;
  couche qui change au `TO` et **pas** au `MO` ; batterie de l'autre moitié qui
  apparaît ≤ 30 s ; frappe pendant le rafraîchissement sans perte (verrou) ;
  veille → image gelée, réveil → écran vivant.
- Contrat : `COMPORTEMENTS.md` (purs + `[smoke:Écrans memory-LCD]`).

## Risques

- Protocole/orientation du panneau sans datasheet vérifiée : traité par le
  bring-up damier en étape 1, isolé du reste.
- Contention SPI avec la radio : toute transaction écran sous le verrou radio ;
  une image entière ≈ 3,5 Ko à 1 MHz ≈ 30 ms — on n'écrit que les lignes
  modifiées, jamais pendant la frappe soutenue (le rafraîchissement cède si le
  verrou n'est pas libre sous 5 ms et réessaie au tick suivant).
- Écran gauche (J12) : `board.h` le dit absent ; si le damier ne s'affiche pas
  à gauche, c'est matériel, pas logiciel.

## Hors périmètre

Animations, écrans multiples (STATS/KPM), luminosité, MO/LT à l'écran, la
souris Conchodytes.
