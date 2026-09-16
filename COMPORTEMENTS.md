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

- [test:test_veille] Grâce après un réveil GPIO : pendant 300 ms
  (VEILLE_GRACE_REVEIL_MS, entre 100 ms et 1 s) la carte ne se rendort pas,
  même si l'inactivité — jamais rafraîchie par un réveil sans touche — dit le
  contraire. Une touche à pré-contact lent réveille la carte avant que la
  capture la voie (deux passes vides) ; sans grâce la boucle renvoyait dormir
  en ~15 ms, avant que le pilote recréé ait vu la touche. Un glitch coûte
  300 ms d'éveil, pas 15 s de radio. Tient au débordement du compteur.
- [smoke:Première touche après veille] UN SEUL sommeil par board : le chemin
  V2D (v2d_sleep.c, OLED + radio, sonde USB toutes les 3 s) est EXCLU des
  boards à veille B7. Depuis l'écran de la gauche (2026-09-14) la garde
  « sans-fil + écran » le compilait aussi sur elle : sur un réveil à capture
  vide, V2D détruisait le pilote, coupait la radio, se rendormait, recréait
  tout à son réveil — la touche de réveil passait dans ce trou (deux
  matrix_setup à 30 ms d'écart au journal, 2026-09-16). Console au réveil :
  un seul « matrix_setup ».
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
  Une capture VIDE journalise les deux passes brutes (« capture vide :
  passe1=… passe2=… ») : rebond (une passe pleine), pré-contact ou fantôme (les
  deux vides) se distinguent au journal — « touche de réveil perdue sur la
  gauche, depuis toujours » (2026-09-16) se chasse avec ça, pas à l'oreille.
  Diagnostic de banc en cours (à retirer une fois tranché) : sur capture vide,
  relecture toutes les 10 ms pendant 150 ms, le journal dit à quel délai une
  touche apparaît (« JAMAIS » = réveil tardif ou glitch ; 10-20 ms = lecture
  précoce fausse ; 100 ms+ = appui suivant) — la gauche voit 4 appuis sur 5,
  le premier, celui qui réveille, manque partout. Et un chronomètre de la
  FENÊTRE AVEUGLE au journal (« chrono entree : radio / pilote / armement /
  jusqu'au sommeil », « chrono sortie : sommeil -> capture ») : entre la
  destruction du pilote et le sommeil réel, puis entre le réveil et la
  capture, une touche n'est ni balayée ni capable de réveiller — 160 à
  570 ms d'éveil autour d'un sommeil vus au tick, à localiser. Mesuré : entrée
  9 ms, sortie 6-8 ms (timer), ce n'est pas là. La même ligne journalise
  désormais les niveaux BRUTS des lignes à la toute première instruction après
  le réveil et le registre d'état GPIO (« lignes a la sortie=0x.. ; broches
  du reveil=0x.. ») : une ligne déclencheuse déjà basse à la sortie = réveil
  GPIO lent ; haute mais invisible à la capture = capture fausse. Un tap léger
  après une longue pause ne réveillait pas la gauche, une pression tenue si.
  Mesuré le 2026-09-16 11:03 : ligne du « a » (GPIO2) déclencheuse du réveil
  mais BASSE 7 ms après, touche lisible seulement à +36 ms — signature d'un
  niveau à la limite du seuil (COL 3,3 V → 1N4148W → ligne ~2,6-2,7 V, seuil
  haut S3 2,475 V). Mesuré par ADC1 (diagnostic retiré ensuite, il passait la
  broche en analogique) : 2885 mV sur la ligne pendant un appui tenu — marge
  réelle, hypothèse écartée. Reste observé et non expliqué par le firmware :
  un premier appui après une longue pause vu nulle part (ni réveil, ni
  capture, ni pilote) ; piste switch (premier contact hésitant), à départager
  en changeant la touche de position.
- [test:test_wake_grace] La grâce laissée au pilote recréé au réveil couvre
  toujours son anti-rebond (debounce × intervalle + 2 balayages), plancher
  10 ms, plafond 50 ms. Un `vTaskDelay(1)` (entre ~0 et 10 ms selon la phase)
  relâchait à tort une touche tenue — tap de Super, Super+F perdu.
- [test:test_veille] Le light sleep vient en 15 s (entre 5 et 20 s) : éveillée
  et oisive la carte tire ~28 mA à 160 MHz (datasheet v2.2 table 5-9 p. 67)
  contre 0,24 mA endormie — à 60 s, une journée de frappe entrecoupée de pauses
  perdait ~0,2 V (2026-09-15). Le réveil sur touche est le chemin nominal, pas
  une exception.
- [smoke:Éveil oisif] Les moitiés tournent en fréquence dynamique
  (CONFIG_PM_ENABLE, esp_pm) : 160 MHz tant qu'une tâche travaille, 40 MHz
  (XTAL, PLL coupée) dès que les deux cœurs sont oisifs — 27,6 → 13,2 mA
  (datasheet v2.2 table 5-9 p. 67). Sous DFS : la radio acquitte à ≥ 98 %,
  l'écran se rafraîchit, la console UART0 reste lisible (esp_pm la passe sur
  XTAL), l'UART du lien TRRS est sur XTAL, la veille et le réveil sont
  inchangés ; un hôte USB monté tient l'APB à 80 MHz (verrou) et suspend les
  sommeils automatiques ; un branchement USB à froid pendant l'oisiveté
  ÉNUMÈRE (banc 2026-09-16 : cafe:4003 vu, route=USB, verrou pris — le seul
  échec observé était un câble de charge seule). Journal au boot : « DFS
  actif : 160 MHz en travail, 40 MHz oisif ».
- [smoke:Éveil oisif] Au repos, le balayage de la matrice S'ARRÊTE
  (keyboard_button en économie d'énergie : gptimer stoppé, colonnes tenues
  hautes, interruption sur les lignes qui le relance au premier appui, premier
  balayage < 1 ms). Sans cela le processeur sortait d'oisiveté 1000 fois par
  seconde et le DFS ne descendait jamais. Le maintien (gpio_hold) que ce mode
  pose sur les colonnes est LEVÉ avant toute conduite hors pilote (capture au
  réveil, armement de veille, recréation) : sinon une touche tenue se lit sur
  toute sa rangée. L'ISR du pilote n'est pas en IRAM (elle appelle du code
  flash) : une touche pressée pendant une écriture NVS attend quelques ms au
  lieu de planter. Le tick LVGL passe à 50 ms, la tâche dort jusqu'à 500 ms.
- [smoke:Première touche après veille] Revue gauche/droite du 2026-09-16 :
  AUCUNE statistique de frappe sur les moitiés (CONFIG_KASE_KEY_STATS=n :
  ni comptage, ni bigrammes, ni NVS — « pas de stats sur le clavier, au mieux
  sur le dongle » ; une écriture flash coupe le cache et arrête balayage et
  émission — 21 sauvegardes en une matinée à gauche, zéro à droite), et la
  gauche ÉTEINT sa radio en veille comme la droite
  (kbd_relay_sleep_prepare / wake_restore autour du light sleep : la puce
  repart ~5 ms avant la capture, comme la droite).
- [smoke:Éveil oisif] Au repos, presque rien ne réveille le processeur : le
  relais radio de la gauche passe à 100 ms (10 ms dès qu'une touche est tenue,
  une réparation bornée en cours, une sync, ou que la gauche ÉCOUTE la droite
  réémise en route USB — [test:test_kbd_refresh] `kbd_relay_cadence_ms` : à
  100 ms ce tick, qui vide la FIFO de réception, avalait les appuis brefs de la
  droite en USB (régression b545e2aa, banc 2026-09-16) ; un changement le
  réveille aussitôt) ; la tâche de rafraîchissement de la droite à 100 ms (20 ms
  touche tenue, notifiée par le balayage sur changement) ; le lien TRRS à
  100 ms tant que le 5 V est mort et l'USB absent (10 ms en poignée de main) ;
  le verrou USB du DFS suit les événements TinyUSB, plus de poll ; le
  battement de coeur à 10 s. Frappe, réparations (ACK ≥ 98 %) et réveil
  inchangés — c'est le smoke DFS qui le vérifie.
- [test:test_keyboard_cadence] La tâche clavier tourne à 10 ms tant qu'une
  minuterie peut courir — moins de 1,5 s depuis la dernière frappe (couvre
  tap-hold et tap-dance 200 ms, leader 1000 ms), hôte USB présent, mode test
  matrice — et à 100 ms au repos ; un changement de matrice la notifie, la
  première touche n'attend jamais. À 100 Hz sa boucle de 10 ms laissait UN
  tick libre quand le light sleep automatique en exige trois : mode SLEEP 92 %
  du temps oisif et light_sleep_counts = 0 (banc 2026-09-16).
- [smoke:Éveil oisif] Les moitiés DORMENT ENTRE LES TOUCHES : tickless idle
  (CONFIG_FREERTOS_USE_TICKLESS_IDLE) + light sleep automatique d'esp_pm dès que
  les deux cœurs sont oisifs plus de 30 ms — au repos ~9 sommeils/s (cadences
  de 100 ms), le battement de coeur de banc en fait foi (CONFIG_PM_PROFILING :
  « light_sleep_counts » qui grimpe, rejets à 0). En sommeil automatique les
  broches du nRF24 sont tenues (CE bas, CSN et IRQ hauts) et l'écran suit un
  rafraîchissement LVGL de 200 ms. La veille B7 à 15 s reste le seul chemin
  vers l'étage long et le sommeil profond ; frappe, ACK, écran, console et
  réveil inchangés.
- [smoke:Éveil oisif] Le battement de coeur de la DROITE (half_link, 10 s)
  porte les mêmes témoins de banc que celui de la gauche quand
  CONFIG_PM_PROFILING est posé : modes et verrous esp_pm, alarmes esp_timer
  armées, et le temps CPU par tâche si les statistiques FreeRTOS sont
  compilées. Mesuré : la droite ne dort pas entre les touches pendant ses
  ~10 premières secondes après un démarrage (116 réveils/s par cœur au lieu
  de 60-80, TinyUSB hors de cause : son temps CPU ne bouge plus après l'init),
  puis ~100 sommeils par 10 s, y compris avant sa première veille B7. Un
  démarrage ne suit qu'un sommeil profond ou un flash : ≤ 15 s à 13 mA,
  assumé et non poursuivi. Aucun effet hors banc.
- [smoke:Une nuit sur batterie] Une moitié tient une nuit sur batterie : de
  l'ordre du centième de volt perdu (244 µA), pas 0,2 V (= ~20 mA : elle n'a
  pas dormi — 2026-09-12 gauche, 2026-09-15 encore). Pour le LIRE : chaque
  réveil journalise « reveil apres N s de sommeil (cause=…) — cumul : n
  sommeils, X s dormies sur Y s », et le battement de coeur porte
  « inactif=… dormi=X s/n » ; une nuit sans sommeil se lit sans multimètre.
- [smoke:Une nuit sur batterie] Le sommeil PROFOND est atteignable : un réveil
  par timer au seuil profond (4 h moins l'étage léger) bascule en deep sleep
  sans passer par une frappe — l'inactivité n'étant évaluée qu'éveillé, la
  carte restait en light sleep jusqu'à une touche (« il ne part jamais en
  deep sleep », 2026-09-15). Un réveil GPIO désarme le timer.
- [smoke:Une nuit sur batterie] En veille, CS, SCK et MOSI de l'écran sont
  tirés BAS (config de sommeil des GPIO), jamais flottants sur les entrées
  CMOS du panneau — l'ESP isole ses broches en light sleep.

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
- [smoke:Fusion — moteur local dormant] Hors USB, le moteur LOCAL de la gauche
  ne tourne pas du tout : le callback de balayage émet la matrice brute au
  dongle, mémorise l'état, note l'activité et s'arrête (ni rapport, ni
  tap-hold, ni combos, ni HID muet). USB branché → la route bascule et le
  moteur reprend au balayage suivant, keymaps déjà chargées au boot. Le mode
  test matrice (CDC) garde la main. « Ne charger le keymap local qu'avec
  l'USB » (2026-09-16) : c'est l'exécution qu'on conditionne, pas le code.

- [NON GARDÉ] Le moteur du dongle ne joue que l'état COURANT de chaque moitié
  à chaque cycle (10 ms) : une transition écrasée avant lecture (appui +
  relâchement, ou relâchement + ré-appui entre deux cycles) est un tap perdu
  ou fondu. Un compteur `transitions_ecrasees` (CDC RF_STATUS[27..30], ligne
  « transition ecrasee » au journal) dit si ça arrive ; s'il reste à 0 pendant
  un épisode de touches perdues, le coupable est ailleurs. Mesure avant refonte
  (file d'états) — 2026-09-15. Confirmé le 2026-09-16 : 83 écrasements en une
  matinée, « oooo » → 2 o, la gauche ayant vu et émis les 4 (journal + 0 refus
  radio). RF_STATUS[31..34] donne l'écart MAXIMAL entre deux tours du moteur
  depuis la dernière lecture : un tap de 70 ms n'est écrasé que si le moteur a
  dormi 70 ms — c'est ce blocage qu'il faut nommer avant de refondre. Et
  RF_STATUS[35..42] compte les rapports clavier USB partis / refusés (point
  d'accès muet) et les reprises de bus demandées / ratées : le maillon
  dongle→hôte se lit sans console (le 09:40 du 2026-09-16 : « aa » vu et émis
  par la gauche, reçu par le dongle, 0 écrasement, écart moteur 13 ms — et
  rien à l'écran).

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
  allumé avant le premier ACK ; jauge et tension locales, la tension
  STABILISÉE 30 s — [test:test_memlcd_model] une valeur différente de
  l'affichée s'affiche quand elle a tenu 30 s : l'oscillation ADC ne tient
  jamais, une dérive lente finit toujours par tenir (une hystérésis autour de
  l'affiché avait figé 4,2 V toute une nuit, 2026-09-15)), centre (GAUCHE : nom de couche en
  lignes de 4 + « Ln » ; DROITE : logo Niphargus 60 px centré, généré par
  scripts/gen_logo_memlcd.sh). PAS de batterie de l'autre moitié : décision
  utilisateur du 2026-09-14, et le canal ACK qui l'aurait portée (trame
  DISPLAY) a été retiré avec — l'ACK reste nu hors sync. L'écran ne se réécrit
  que si un champ affiché change ; une image refusée par le bus occupé est
  repoussée au tick suivant, jamais perdue ; seuil et envoi sont sous un même
  mutex (flush LVGL et relance ne transposent jamais le même tampon en même
  temps — sinon des lignes partent blanches : « une partie de l'écran
  s'efface », droite, 2026-09-14).
