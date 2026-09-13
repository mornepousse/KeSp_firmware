/* Layout physique du dongle en mode FUSION — redirigé vers la GAUCHE.
 *
 * Même logique que board_keymap.c : une seule géométrie 2D, celle de la gauche
 * (boards/niphar_layout.inc). Le `#include "../niphar_layout.inc"` interne se
 * résout relativement à niphar_left/board_layout.c, donc pointe bien vers
 * boards/niphar_layout.inc quel que soit l'incluant. Compilé uniquement sous
 * KASE_DONGLE_FUSION (CMake). */
#include "../niphar_left/board_layout.c"
