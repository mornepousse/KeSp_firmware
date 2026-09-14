# Contrat de comportements — KeSp

Ce que le firmware doit faire, et ce qui le garde. À lire **avant de toucher une
source**. Chaque comportement est tagué par sa garde : `[test:X]` (X est dans un
fichier de test), `[smoke:X]` (X est un item de `docs/HARDWARE_SMOKE_TEST.md`,
vérifié à la main avant chaque release), ou `[NON GARDÉ]` — l'aveu, compté dans
`.tripwire-nongardes`, qui n'a le droit que de baisser.

Une source modifiée sans test ni ligne ici est une question sans réponse : le
hook par édition la pose, le Stop bloque. Y répondre = un test, ou une ligne.

## Radio — lien split

- [test:test_repos_ne_reemet_pas] Au repos, le rapport HID n'est pas réémis.
  La réémission est bornée (100 paquets/s), s'arme au changement et se tait
  ensuite. Sans ça, la spirale de réémission saturait le lien. En fusion, la
  même réémission bornée est armée à chaque changement de MATRICE de la gauche
  (dernier bitmap, même vide) : une trame de changement refusée par l'ESB
  (~1 %) est répétée 5 × à 10 ms puis silence — un appui bref n'a plus une
  seule chance de passer (Super+Q avalé, banc 2026-09-13).
- [test:test_rf_status_cadence] Le status RF respecte sa période : rien avant,
  une émission à la période. Trois maillons perdaient des frappes pour la même
  raison — une cadence qui n'attendait pas.
- [test:test_half_tx_repeat] La droite répète chaque changement de matrice un
  nombre BORNÉ de fois (HALF_TX_REPEATS ticks) puis se tait : une trame de
  changement refusée par l'ESB n'est plus perdue, et le repos reste muet (R1).
  La règle de maintien (réaffirmation à 100 ms) reste intacte derrière.
- [smoke:NRF ne se wedge pas après 5 min] La droite a un chien de garde radio.
  Un nRF24 figé est relancé ; il n'y a plus de mort permanente du lien.

<!-- D'autres invariants radio restent à inscrire (Mae, 2026-09-13). -->

## Veille — réveil

- [NON GARDÉ] Un réveil est une activité : la carte ne se rendort pas dans les
  10 ms qui suivent. Candidat n°1 pour un test hôte avec `host_clock`.
- [NON GARDÉ] Au réveil, la réception RF est réarmée (`rf_driver_power_up` ne
  touche pas à CE). Sinon la gauche repart alimentée mais sourde.
- [smoke:Première touche après veille] La touche qui réveille la carte est
  capturée, émise et réconciliée — jamais perdue. Sous fusion, la gauche
  émet l'appui capturé au réveil (matrix_wake_capture) et son relâchement à la
  réconciliation, avec l'émetteur du callback : le scanner recréé ne voit pas de
  changement, lui seul ne l'aurait jamais émis (première touche avalée, banc
  2026-09-13). Une touche de réveil TENUE n'est jamais relâchée à tort :
  la réconciliation attend le premier événement du pilote, pas un tick nu.
  Un premier front lu pendant le rebond (capture vide sur réveil GPIO) est
  relu 5 ms plus tard avant d'être déclaré fantôme — pas de rendormissement
  qui avalerait un tap bref ; un vrai glitch (deux captures vides) reste rejeté.
- [test:test_wake_grace] La grâce laissée au pilote recréé au réveil couvre
  toujours son anti-rebond (debounce × intervalle + 2 balayages), plancher
  10 ms, plafond 50 ms. Un `vTaskDelay(1)` (entre ~0 et 10 ms selon la phase)
  relâchait à tort une touche tenue — tap de Super, Super+F perdu.
- [smoke:Une nuit sur batterie] La moitié droite tient une nuit sur batterie,
  moins de 0,2 V perdus.

## Entrées — matrice et rapport HID

- [test:test_kp_slot_recycle_ne_gele_pas_le_keycode] Un slot de touche recyclé
  ne conserve pas le keycode du cycle précédent. Sinon une touche relâchée
  continuait d'émettre l'ancien code.
- [test:test_take_consumes_the_signal] Un front de matrice n'est jamais perdu
  pendant la lecture : le signal est pris, consommé, jamais écrasé par la
  lecture suivante.
- [test:test_first_report_always_sent] Un rapport HID refusé par l'hôte n'est
  pas jeté — il est renvoyé. Sinon un modificateur restait collé.
- [test:test_th_lt_oob_wins_recompute_after_valid_release] Une LT (layer-tap)
  hors bornes ne peut pas gagner le recalcul de couche : seul un relâchement
  valide y participe.

## Niphargus — poignée de main 5 V

- [test:test_lost_probe_eventually_reprobes] Après une sonde perdue, la poignée
  de main 5 V re-sonde. Elle ne reste pas bloquée sur un échec.

## Fusion — routage des moteurs

- [test:test_gauche_par_usb] Sans hôte USB, la gauche ne tape pas en local :
  elle émet son brut, le dongle tape. En route RF, ses émetteurs HID USB
  (`hid_transport.c`) se taisent — pas d'attente d'EP ni de « report not sent
  (EP busy) » à chaque frappe vers un USB sans hôte.

## Fusion — garde-fou de sync config

- [test:test_rf_status_config_fp] L'empreinte CRC-32 de la keymap voyage dans
  PKT_TYPE_STATUS (round-trip), et vaut 0 si absente (rétrocompatible avec un
  firmware pré-empreinte). La gauche l'annonce ; le dongle la lit.
- [test:test_coherence_once_par_changement] Le dongle ne signale la cohérence
  (accord ou divergence) qu'au CHANGEMENT d'empreinte, pas à chaque trame d'état
  (~1/s) — sinon la console serait noyée.
- [test:test_fp_match] Une empreinte nulle (« pas encore annoncée ») ne vaut
  jamais un accord : 0 vs 0 reste incohérent.
- [smoke:Divergence de config signalée] En sans-fil, si la keymap du dongle
  diverge de celle de la gauche, le dongle le journalise et l'expose par CDC
  (KS_CMD_CONFIG_COHERENCE : own_fp/left_fp/age/match) — le contrôleur peut
  avertir. Sinon deux moteurs taperaient différemment en silence.

## Fusion — sync auto de la keymap (ACK payload)

- [test:test_keymap_sync_frames] Les trames BEACON/CHUNK/REQ survivent à
  l'encode/decode et tiennent dans un ACK payload nRF24 (≤ 32 o) ; la géométrie
  40 × 28 = 1120 = keymap est verrouillée, sans chunk partiel.
- [test:test_keymap_sync] Le réassembleur n'accepte que le prochain chunk
  attendu ; doublons et hors-séquence sont ignorés sans rien écrire — un ACK
  payload rejoué ne corrompt jamais la keymap en cours de réception.
- [smoke:Canal retour ACK payload] Le PTX (gauche) lit la charge utile portée
  par l'ACK après TX_DS, et sa FIFO RX ne s'encrasse jamais (vidée si non lue ou
  corrompue). Sans ça, trois ACK chargés suffisent à rendre le canal retour
  muet en silence.
- [smoke:Sync keymap sans câble] Une divergence dongle↔gauche se résorbe SEULE
  par radio : le dongle glisse la keymap dans les ACK des émissions normales de
  la gauche (balise, puis chunks à la demande), la gauche réassemble, enregistre
  en NVS et annonce la nouvelle empreinte ; `match` repasse à 1 sans brancher la
  gauche, et la balise se tait aussitôt (coût nul une fois synchronisé). Un
  maintien de touche garde la priorité sur le pull (jamais de touche relâchée à
  tort pour une keymap). Le dongle ne charge une charge d'ACK qu'après une trame
  de la GAUCHE (STATUS, SYNC_REQ, MATRIX gauche) — la droite partage le slot et
  la consommerait à vide : la sync converge aussi sous frappe bilatérale.

## Batterie — jauge des moitiés

- [test:test_batt_calc] La tension batterie est convertie depuis le pont 1M/1M
  (V_batt = 2 × V_adc), moyennée, et rejetée hors [2,5 V ; 4,5 V] (0 = inconnu,
  jamais un chiffre faux) ; le SoC est une table Li-ion 16340 bornée et
  monotone ; « pleine » exige un plateau ≥ 4,15 V tenu 2 min avec hystérésis,
  « en charge probable » une hausse ≥ 0,1 V dans une fenêtre de 5 min — une
  décharge ou une dérive lente ne l'est jamais ; une mesure inconnue oublie tout.
- [test:test_rf_status_half_et_charge] STATUS porte l'identité de moitié et
  l'état de charge dans son nibble de flags, sans changer de taille ; une trame
  ancienne se lit gauche / inconnu (rétrocompatible).
- [smoke:Jauge batterie] Les deux moitiés remontent une tension plausible au
  dongle (CDC BATTERY, slots gauche/droite), la droite par un STATUS toutes les
  30 s sans s'empêcher de dormir ; une tension inconnue s'affiche « inconnue »
  (0xFF), jamais 0 V ; en charge, PLEINE apparaît après le plateau.

## Écrans — Sharp memory-LCD des moitiés

- [test:test_memlcd_model] rev8 est une involution (l'adresse de ligne se lit
  CA0 en premier, l'ESP32 émet MSB-first : une inversion fausse = écran muet
  sans erreur) ; le tampon portrait se transpose en 68 lignes × 20 octets, 1 =
  blanc, tampon vide = panneau blanc, pixel (0,0) → ligne 0 colonne 159 ; le
  nom de couche se coupe en 4 caractères × 3 lignes puis « … », jamais zéro
  ligne, NULL sûr ; le modèle ne déclenche un redessin que si un champ AFFICHÉ
  change — chaque redessin est une transaction sur le bus partagé avec la radio.
- [test:test_ecran_memlcd_gauche] La gauche a LE MÊME écran que la droite
  (J12 soudé le 2026-09-14) : CS 14 actif haut, portrait 68×160, et aucun autre
  backend (ROUND/OLED) ne peut être choisi par CMake pour cette moitié.
- [smoke:Écrans memory-LCD] Le CS de l'écran (actif haut) est tenu BAS dès le
  boot des deux moitiés ; le protocole suit l'app note Sharp (lemia doc 6845
  p. 10-12) : mot de commande BRUT (M0 = premier bit clocké), adresse de ligne
  en rev8 (CA0 en premier), 68 lignes × 20 octets (catalogue doc 6844 p. 5 :
  160 × 68, H = sens des données) transposées depuis le portrait 68 × 160 ;
  l'attachement du panneau attend que la radio ait créé le bus SPI (init
  différée) ; toute transaction écran passe sous le verrou du propriétaire de
  la radio et cède si elle est occupée ; la mire de bring-up (cadre, pavé plein
  en HAUT-GAUCHE, damier 8 px) est nette et bien orientée ; l'image reste gelée
  en light sleep et aucun réveil n'est dû à l'écran.
- [smoke:Écrans memory-LCD UI] Les deux moitiés affichent en portrait : bandeau
  (route RF/USB, ▲ « dongle vu » COLLANT — une moitié est muette au repos, un
  indicateur daté clignoterait à chaque STATUS — qui ne tombe qu'après 3
  émissions consécutives sans ACK, jamais sur un refus ESB isolé, et jamais
  allumé avant le premier ACK ; jauge et tension locales avec une hystérésis
  d'un dixième de volt, l'ADC oscillant entre deux dV voisins), centre (GAUCHE : nom de couche en
  lignes de 4 + « Ln » ; DROITE : logo Niphargus 60 px centré, généré par
  scripts/gen_logo_memlcd.sh). PAS de batterie de l'autre moitié : décision
  utilisateur du 2026-09-14, et le canal ACK qui l'aurait portée (trame
  DISPLAY) a été retiré avec — l'ACK reste nu hors sync. L'écran ne se réécrit
  que si un champ affiché change ; une image refusée par le bus occupé est
  repoussée au tick suivant, jamais perdue ; seuil et envoi sont sous un même
  mutex (flush LVGL et relance ne transposent jamais le même tampon en même
  temps — sinon des lignes partent blanches : « une partie de l'écran
  s'efface », droite, 2026-09-14).
