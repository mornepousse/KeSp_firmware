# Sync auto de la keymap dongle→gauche par ACK payload ESB (fusion, phase 3)

Date : 2026-09-13
Statut : design validé (approche A), spec à relire avant plan d'implémentation.

## Contexte et problème

En fusion (`KASE_DONGLE_FUSION`), le dongle exécute le moteur keymap en sans-fil
et sort le HID ; les deux moitiés lui envoient leur matrice brute. La keymap
**propre** de la moitié gauche ne s'exécute que dans un seul mode : **gauche
branchée seule en USB** (standalone, validé phase 2). L'utilisateur s'en sert
occasionnellement.

La config entre par le **dongle** (le seul appareil branché à l'hôte ; l'USB-C de
la gauche est power-only, sans CDC — cf. [[fusion-dongle-et-usb-gauche]]). Le
dongle est donc la **source de vérité**. Le garde-fou de sync (commit `b1db4091`)
DÉTECTE quand la keymap de la gauche diverge de celle du dongle (empreinte CRC-32
annoncée dans `PKT_TYPE_STATUS`, comparée côté dongle, exposée par CDC 0x17), mais
ne CORRIGE pas. Aujourd'hui la correction est manuelle (extraction NVS + SETLAYER).

But : synchroniser la gauche **automatiquement, sans jamais la brancher**, et sans
entamer l'autonomie.

### La contrainte qui a façonné le design

La gauche n'écoute la radio (PRX) qu'en mode USB ; en sans-fil elle est **PTX pur
et sourde** (autonomie : écouter coûte 13 mA, cf. CLAUDE.md, brick B7). Un push
« le dongle émet, la gauche écoute » exigerait donc de brancher la gauche —
rejeté par l'utilisateur.

## Approche retenue : drip par ACK payload

La gauche **parle** déjà au dongle en sans-fil (STATUS ~1/s + MATRIX à chaque
frappe), en ESB avec auto-ACK matériel. Le nRF24 sait glisser une charge utile
DANS cet ACK (`EN_ACK_PAY`). Le dongle (PRX) charge un ACK payload contenant un
morceau de keymap ; quand la trame suivante de la gauche arrive, l'ACK matériel le
lui rapporte. La gauche (PTX) lit l'ACK payload après `TX_DS` et l'applique.

Conséquences :
- **Pas de câble** : ça roule sur le lien sans-fil normal.
- **Pas de mode écoute** : la gauche reste PTX, autonomie intacte.
- **Pas d'événement de changement** : opportuniste et continu **tant que les
  empreintes diffèrent**, silencieux dès qu'elles concordent (coût nul en régime
  synchronisé).
- **Coût énergie négligeable** : lire un ACK payload après une émission déjà
  faite est quasi gratuit.

Alternatives écartées : (B) brancher la gauche en USB — rejeté (câble) ; (C) pas
d'auto-sync, garde-fou seul — rejeté (l'utilisateur veut le standalone-gauche
prêt sans intervention).

## Protocole (pull piloté par la gauche)

Un ACK payload peut se perdre (l'ACK n'est pas lui-même acquitté). Plutôt que le
dongle devine, **la gauche tire** : elle dit ce qu'il lui faut dans son uplink, le
dongle répond dans l'ACK. Auto-correcteur, sans état de retransmission côté dongle.

Keymap = `KEYMAP_BLOB_BYTES` = 1120 o (10 couches × 4 rangées × 14 colonnes × 2).
Découpée en **chunks de 28 o** → 40 chunks (le dernier partiel). ACK payload ≤ 32 o.

- **Balise (dongle → gauche, dans l'ACK, au repos de sync)** : quand le dongle
  voit `match=0` (garde-fou), il place dans l'ACK une balise
  `{type=BEACON, fp_cible, n_chunks}`. Sinon (match=1) il ne place rien : silence.
- **Demande (gauche → dongle, dans l'uplink)** : voyant une balise dont `fp_cible`
  ≠ sa propre empreinte, la gauche entame un pull. Elle indique dans son STATUS
  (champ ajouté) le **prochain chunk voulu** `k`.
- **Chunk (dongle → gauche, dans l'ACK)** : le dongle répond
  `{type=CHUNK, idx=k, data[28]}`. La gauche l'écrit dans un buffer de réassemblage
  de 1120 o, avance `k`.
- **Fin** : quand les 40 chunks sont là, la gauche écrit le blob via le **même
  chemin que `SETLAYER`** (`save_keymaps`), recalcule son empreinte. Au STATUS
  suivant, elle annonce la nouvelle `fp` ; le dongle voit `match=1` → arrête la
  balise. L'empreinte du garde-fou EST l'accusé de bout en bout.

Perte d'un chunk : la gauche ne voit jamais arriver `k`, redemande `k` au tour
suivant. Perte d'une balise : la gauche redemande au STATUS suivant. Aucune
machine à retransmettre côté dongle ; l'empreinte finale prouve l'intégrité.

### Versioning / direction

Le dongle est l'unique point de config (la gauche est power-only), donc il est
toujours autoritativement le plus récent : push **dongle → gauche**
inconditionnel sur divergence. Pas de numéro de version en v1 (YAGNI). Si un jour
on édite la gauche par FTDI, ce serait à réconcilier — hors périmètre.

## Périmètre

- **v1 : keymap seule** (10 couches), ce que l'empreinte couvre déjà.
- Hors v1 : macros, combos, tap-dance, leader, overrides (autres blobs NVS).
  Extension naturelle (mêmes chunks, autre blob + autre empreinte) — plus tard.

## Découpage en unités

Logique pure (testée host, norme TDD) :
- **Encode/decode des trames de sync** (BEACON, CHUNK, la demande dans STATUS) —
  format binaire borné à ≤32 o.
- **Réassembleur** : reçoit des `{idx, data}` dans un ordre quelconque, avec
  doublons ; expose « prochain chunk manquant » et « complet ? ». État pur.
- **Empreinte** : `config_fp_crc32` (déjà là et testée).

Transport (RF, zone « une puce, un propriétaire ») :
- **Driver** : activer `EN_ACK_PAY` (FEATURE bit1 → 0x06) des deux côtés ;
  `rf_driver_load_ack_payload()` (PRX, `W_ACK_PAYLOAD`) ; lecture de l'ACK payload
  après `TX_DS` côté PTX (exposée par `rf_driver_send` ou un getter).
- **Dongle** (rf_rx_task, propriétaire unique) : sur mismatch, charge balise puis
  chunks demandés dans l'ACK, après chaque trame reçue.
- **Gauche** (kbd_relay TX, propriétaire unique) : après émission, lit l'ACK
  payload ; applique balise/chunk ; met le « prochain chunk » dans son STATUS ;
  à complétion, `save_keymaps` + recalcul d'empreinte.

## Plan incrémental, prouvé au banc (RF-critique)

Chaque étape prouvée contre des ACK réels avant la suivante (cf. les trois pannes
silencieuses de CLAUDE.md dans cette zone) :

1. **Driver ACK payload** : le dongle renvoie un ACK payload « hello » connu ; la
   gauche le lit après TX et le logue. Prouve le canal retour.
2. **Balise** : le dongle, sur `match=0`, place `{BEACON, fp, n}` ; la gauche la
   logue.
3. **Un chunk** : la gauche demande le chunk 0 dans son STATUS, le dongle le sert,
   la gauche l'applique — prouver qu'une couche atterrit.
4. **Boucle complète** : 40 chunks, réassemblage, `save_keymaps`, empreinte →
   `match=1` **sans aucun câble**.
5. **Robustesse** : ACK perdus, en pleine frappe, et **zéro trafic une fois
   synchronisé** (la balise cesse).

## Vérification

- Host : `./scripts/check.sh --fast` vert ; nouveaux tests purs (trames +
  réassembleur) rouges avant impl, mordant à la mutation.
- Boards : dongle+fusion / niphar_left+fusion / niphar_right+fusion compilent ;
  les 7 boards par défaut restent verts.
- Banc : dérouler le plan incrémental ; critère final = provoquer une divergence
  (éditer la keymap du dongle) puis, **sans rien brancher**, voir `match` repasser
  à 1 via CDC 0x17 en < 1 min au repos.
- Contrat : `COMPORTEMENTS.md` — comportements gardés par les tests purs +
  `[smoke:*]` pour le bout-en-bout.

## Risques et hypothèses

- **Clones nRF24 et ACK payloads** : tous les clones ne gèrent pas proprement les
  ACK payloads. À éprouver en étape 1 ; repli = approche B (brancher) documentée
  si le silicium ne suit pas.
- **Contention** : charger l'ACK payload ne doit pas perturber le relais de frappe
  ni la réémission droite→gauche. La sync est un régime de fond, bornée, et
  s'éteint dès `match=1`.
- **Hypothèse de direction** : le dongle est toujours le plus récent (config par
  lui seul). Vraie tant que la gauche n'est pas éditée hors-bande.
