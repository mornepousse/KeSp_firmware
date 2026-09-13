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
