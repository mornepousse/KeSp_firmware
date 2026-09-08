/* Choix du niveau de veille (logique pure) — brick B7.
 *
 * Deux etages, et chacun paie sa place :
 *
 *   LEGERE  — CPU en light sleep, etat conserve, reveil en ~1 ms sur la matrice
 *             locale. 240 uA (ESP32-S3 datasheet v2.2, table 5-10, p. 68) plus
 *             4 uA pour le HT7833. Sur une 16340 de ~650 mAh, quatre heures a
 *             ce regime coutent 1 mAh : l'etage est quasiment gratuit, et c'est
 *             ce qui permet de le tenir des heures plutot que des minutes.
 *
 *   PROFONDE — deep sleep, 8 uA, reveil par EXT1 sur les lignes. La RAM est
 *             perdue : le reveil est un redemarrage complet, mesure a 683 ms
 *             au banc. On ne l'atteint donc qu'apres des heures d'absence, ou
 *             ce delai ne se remarque pas.
 *
 * L'ULP est exclu : 170 uA (meme table), soit plus de trois fois la cible de
 * 50 uA a lui seul. Le « scan RTC » annonce dans le design ne peut pas tenir.
 *
 * ⚠ La radio est eteinte des l'etage LEGERE — ecouter coute 13,1 mA
 * (nRF24L01+ PS v1.0, table 4, p. 14), deux ordres de grandeur au-dessus de
 * tout budget de veille. Une moitie endormie n'entend donc PAS l'autre : le
 * reveil se fait sur sa propre matrice. C'est une contrainte de la puce, pas
 * un choix d'implementation.
 */
#include "test_framework.h"
#include "../main/power/veille.h"

#define LEGERE   60000u        /* 1 min */
#define PROFONDE 14400000u     /* 4 h  */

static void test_activite_recente_ne_dort_pas(void)
{
    TEST_ASSERT(veille_niveau(0, false, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "on vient de taper");
    TEST_ASSERT(veille_niveau(LEGERE - 1, false, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "juste avant le seuil : toujours eveille");
}

static void test_etage_leger(void)
{
    TEST_ASSERT(veille_niveau(LEGERE, false, LEGERE, PROFONDE) == VEILLE_LEGERE,
                "au seuil : light sleep");
    TEST_ASSERT(veille_niveau(PROFONDE - 1, false, LEGERE, PROFONDE) == VEILLE_LEGERE,
                "jusqu'a la derniere milliseconde avant le sommeil profond");
}

static void test_etage_profond(void)
{
    TEST_ASSERT(veille_niveau(PROFONDE, false, LEGERE, PROFONDE) == VEILLE_PROFONDE,
                "au seuil : deep sleep");
    TEST_ASSERT(veille_niveau(0xFFFFFFFFu, false, LEGERE, PROFONDE) == VEILLE_PROFONDE,
                "et bien au-dela");
}

static void test_le_blocage_prime_sur_tout(void)
{
    /* LE test de ce fichier. Un verrou — USB branche, touche tenue, mise a jour
     * en cours — doit interdire TOUTE veille, y compris apres des heures. Se
     * tromper ici endort un clavier en pleine utilisation. */
    TEST_ASSERT(veille_niveau(LEGERE, true, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "bloque : pas de light sleep");
    TEST_ASSERT(veille_niveau(PROFONDE * 2, true, LEGERE, PROFONDE) == VEILLE_AUCUNE,
                "bloque : pas de deep sleep non plus, meme apres huit heures");
}

static void test_les_seuils_sont_ordonnes(void)
{
    /* Un profond plus court que le leger rendrait l'etage leger inatteignable,
     * et le clavier passerait directement au redemarrage de 683 ms. */
    TEST_ASSERT(VEILLE_LEGERE_MS < VEILLE_PROFONDE_MS,
                "le seuil leger vient avant le profond");
    TEST_ASSERT(VEILLE_PROFONDE_MS >= 3600000u,
                "le sommeil profond n'arrive pas avant une heure d'absence");
}

void test_veille(void)
{
    printf("\n-- choix du niveau de veille (B7) --\n");
    test_activite_recente_ne_dort_pas();
    test_etage_leger();
    test_etage_profond();
    test_le_blocage_prime_sur_tout();
    test_les_seuils_sont_ordonnes();
}
