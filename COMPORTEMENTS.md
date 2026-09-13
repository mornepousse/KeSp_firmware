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
  ensuite. Sans ça, la spirale de réémission saturait le lien.
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
- [NON GARDÉ] La touche qui réveille la carte est capturée, émise et
  réconciliée — jamais perdue.
- [smoke:Une nuit sur batterie] La moitié droite tient une nuit sur batterie,
  moins de 0,2 V perdus.

## Niphargus — poignée de main 5 V

- [test:test_lost_probe_eventually_reprobes] Après une sonde perdue, la poignée
  de main 5 V re-sonde. Elle ne reste pas bloquée sur un échec.
