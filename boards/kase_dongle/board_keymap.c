/* Keymap par défaut du dongle en mode FUSION — redirigée vers la GAUCHE.
 *
 * En mode KASE_DONGLE_FUSION le dongle fait tourner le moteur keymap et partage
 * la keymap par défaut de la moitié gauche : une seule source de vérité, jamais
 * deux à diverger (règle « même code » du design). Ce fichier n'est compilé que
 * lorsque le moteur l'est (CMake : NOT CONFIG_KASE_NO_KEYMAP_ENGINE, vrai sur le
 * dongle uniquement sous fusion). Hors fusion, il n'entre pas dans le build.
 *
 * board.h se résout vers celui du dongle (dimensions de fusion), pas celui de la
 * gauche — c'est voulu : mêmes valeurs, brochage du dongle.
 *
 * Design : docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md */
#include "../niphar_left/board_keymap.c"
