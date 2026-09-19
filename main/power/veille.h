/* Veille du Niphargus — brick B7.
 *
 * Deux étages, parce qu'aucun seul ne convient :
 *
 *   LEGERE   light sleep, ~240 µA (ESP32-S3 datasheet v2.2, table 5-10, p. 68)
 *            plus 4 µA pour le HT7833. L'état est conservé, le réveil prend
 *            ~1 ms. Sur une 16340 de ~650 mAh, quatre heures à ce régime
 *            coûtent 1 mAh — c'est ce qui permet de tenir cet étage des HEURES
 *            plutôt que des minutes, et donc d'avoir un clavier qui repart
 *            instantanément toute la journée.
 *
 *   PROFONDE deep sleep, 8 µA (même table), réveil par EXT1 sur les lignes.
 *            La RAM est perdue : le réveil est un redémarrage complet, mesuré
 *            à 683 ms au banc. On ne l'atteint donc qu'après des heures, où ce
 *            délai passe inaperçu — et là, l'autodécharge de la cellule domine.
 *
 * L'ULP est écarté : 170 µA à lui seul (même table), soit plus de trois fois la
 * cible de 50 µA. Le « scan RTC » annoncé dans le design ne peut pas la tenir.
 *
 * ⚠ LA RADIO EST ÉTEINTE DÈS L'ÉTAGE LÉGER. Écouter coûte 13,1 mA
 * (nRF24L01+ PS v1.0, table 4, p. 14), deux ordres de grandeur au-dessus de
 * tout budget de veille, et le nRF24 n'a pas de mode d'écoute basse
 * consommation. Une moitié endormie n'entend donc PAS l'autre : chacune se
 * réveille sur SA matrice. Conséquence assumée — après une longue absence, la
 * première frappe doit être sur la moitié gauche, celle qui parle à l'hôte.
 * C'est une contrainte de la puce, pas un choix d'implémentation ; sonder
 * coûterait plus cher que de ne pas dormir (un réveil CPU de 3 ms toutes les
 * 500 ms vaut déjà 240 µA).
 *
 * Logique pure, testée host dans test/test_veille.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VEILLE_AUCUNE = 0,   /* rester éveillé */
    VEILLE_LEGERE,       /* light sleep, réveil GPIO matrice */
    VEILLE_PROFONDE,     /* deep sleep, réveil EXT1 */
} veille_t;

/* Seuils par défaut. Le léger est COURT : éveillée et oisive la carte tire
 * ~28 mA à 160 MHz (ESP32-S3 datasheet v2.2, table 5-9, p. 67) contre 0,24 mA
 * endormie — chaque seconde d'attente vaut cent secondes de sommeil, et à 60 s
 * une journée de frappe perdait ~0,2 V (2026-09-15). Le profond se compte en
 * heures, puisque l'étage léger ne coûte presque rien et évite le redémarrage. */
#define VEILLE_LEGERE_MS     15000u      /* 15 s   */
#define VEILLE_PROFONDE_MS   14400000u   /* 4 h    */

/* `bloque` interdit toute veille : USB branché, mise à jour en cours, ou toute
 * raison de rester éveillé. Il prime sur les deux seuils — une durée
 * d'inactivité, si longue soit-elle, ne doit jamais endormir un clavier qu'on
 * est en train d'utiliser autrement. */
#ifndef TEST_HOST
/* Firmware. veille_pas() applique la décision : elle bloque jusqu'au réveil
 * pour l'étage léger, et ne revient jamais pour le profond. */
void veille_liberer_gpio(void);   /* au démarrage, AVANT matrix_setup() */
void veille_legere_entrer(void);
void veille_profonde_entrer(void);
void veille_pas(uint32_t inactif_ms, bool bloque);
/* Seuil de l'étage léger, en ms : CONFIG_KASE_VEILLE_LEGERE_S par défaut,
 * VEILLE_LEGERE_CRITIQUE_MS quand la batterie est critique (veille_task). */
uint32_t veille_seuil_legere_ms(void);
void     veille_seuil_legere_set(uint32_t ms);
#define VEILLE_LEGERE_CRITIQUE_MS 5000u   /* borne basse de test_veille : [5 ; 20] s */
/* Diagnostic : quand l'inactivité dépasse le seuil léger mais que la veille
 * est bloquée, dire PAR QUOI, au plus une fois par 30 s. Une nuit à 20 mA au
 * lieu de 244 µA (0,2 V perdus sur la gauche, 2026-09-12) n'a laissé aucune
 * trace parce que rien ne journalisait un sommeil refusé. */
/* Bilan depuis le boot : nombre de sommeils légers et temps total dormi (ms,
 * mesuré à l'esp_timer, qui suit le RTC). Sert au battement de coeur. */
void veille_bilan(uint32_t *sommeils, uint32_t *dormi_ms);
#endif

/* Grâce après un réveil GPIO : pendant VEILLE_GRACE_REVEIL_MS on ne se rendort
 * pas, même si l'inactivité (jamais rafraîchie par un réveil sans touche) dit
 * le contraire. Une touche à pré-contact lent réveille la carte avant que la
 * capture la voie ; le pilote recréé la verra dans ces 300 ms et l'émettra.
 * Un glitch coûte 300 ms d'éveil (~2 µAh), pas 15 s de radio.
 * dernier_reveil_ms = 0 : jamais réveillé, pas de grâce. Soustraction non
 * signée : tient au débordement du compteur. */
#define VEILLE_GRACE_REVEIL_MS 300u
static inline bool veille_en_grace(uint32_t now_ms, uint32_t dernier_reveil_ms, uint32_t grace_ms)
{
    if (dernier_reveil_ms == 0) return false;
    return (uint32_t)(now_ms - dernier_reveil_ms) < grace_ms;
}

static inline veille_t veille_niveau(uint32_t inactif_ms, bool bloque,
                                     uint32_t seuil_legere_ms,
                                     uint32_t seuil_profonde_ms)
{
    if (bloque)                               return VEILLE_AUCUNE;
    if (inactif_ms >= seuil_profonde_ms)      return VEILLE_PROFONDE;
    if (inactif_ms >= seuil_legere_ms)        return VEILLE_LEGERE;
    return VEILLE_AUCUNE;
}
