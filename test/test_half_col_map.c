/* Colonne keymap d'une touche venue de la moitie distante (logique pure).
 *
 * La gauche indexe keymaps[layer][row][col] sur 14 colonnes : 0-6 pour son
 * propre balayage, 7-13 pour ce que la droite lui envoie. Reste a savoir DANS
 * QUEL SENS ranger les 7 colonnes de la droite, et c'est la que le materiel
 * decide.
 *
 * Les deux moities sont le meme PCB retourne. La colonne 0 de la gauche est sa
 * touche la plus a GAUCHE ; par symetrie, la colonne 0 de la droite est sa
 * touche la plus a DROITE. Un decalage naif (col + 7) range donc la moitie
 * droite a l'envers : on tape la rangee de repos et il sort ";lkjh" au lieu de
 * "hjkl;". C'est le defaut constate au banc le 2026-09-07.
 *
 * Le miroir est une propriete du CABLAGE, pas du protocole : la droite emet ses
 * coordonnees physiques et n'a pas a savoir ou elle est posee. La conversion
 * appartient donc au maitre, et le drapeau vient de son board.h. */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"

#define COLS 7

static void test_sans_miroir_c_est_un_simple_decalage(void)
{
    /* Le cas d'une moitie cablee dans le meme sens que la gauche. */
    TEST_ASSERT(half_col_to_keymap(0, COLS, false) == 7,  "col 0 -> 7");
    TEST_ASSERT(half_col_to_keymap(6, COLS, false) == 13, "col 6 -> 13");
}

static void test_avec_miroir_l_ordre_s_inverse(void)
{
    /* LE test de ce fichier : la colonne 0 de la droite est la plus a droite
     * du clavier, donc la derniere colonne de la keymap. */
    TEST_ASSERT(half_col_to_keymap(0, COLS, true) == 13, "col 0 (bord exterieur) -> 13");
    TEST_ASSERT(half_col_to_keymap(6, COLS, true) == 7,  "col 6 (bord interieur) -> 7");
    TEST_ASSERT(half_col_to_keymap(3, COLS, true) == 10, "la colonne du milieu est fixe");
}

static void test_le_mapping_reste_une_bijection(void)
{
    /* Quel que soit le sens, les 7 colonnes distantes doivent couvrir
     * exactement 7..13, sans trou ni collision — sinon une touche en ecrase
     * une autre ou n'est jamais atteignable. */
    for (int miroir = 0; miroir <= 1; miroir++) {
        int vu[2 * COLS];
        memset(vu, 0, sizeof(vu));
        for (uint8_t c = 0; c < COLS; c++) {
            uint8_t k = half_col_to_keymap(c, COLS, miroir != 0);
            TEST_ASSERT(k >= COLS && k < 2 * COLS, "reste dans la moitie distante");
            TEST_ASSERT(!vu[k], "aucune collision");
            vu[k] = 1;
        }
        for (uint8_t k = COLS; k < 2 * COLS; k++)
            TEST_ASSERT(vu[k], "aucun trou");
    }
}

static void test_le_miroir_est_le_decalage_de_la_colonne_symetrique(void)
{
    /* Propriete qui relie les deux branches au lieu de les tester separement :
     * mettre la moitie en miroir revient exactement a decaler la colonne
     * symetrique. Si l'une des deux formules derive un jour, celle-ci mord. */
    for (uint8_t c = 0; c < COLS; c++)
        TEST_ASSERT(half_col_to_keymap(c, COLS, true)
                    == half_col_to_keymap((uint8_t)(COLS - 1 - c), COLS, false),
                    "miroir(c) == decalage(symetrique de c)");
}

void test_half_col_map(void)
{
    printf("\n-- colonne keymap de la moitie distante (miroir) --\n");
    test_sans_miroir_c_est_un_simple_decalage();
    test_avec_miroir_l_ordre_s_inverse();
    test_le_mapping_reste_une_bijection();
    test_le_miroir_est_le_decalage_de_la_colonne_symetrique();
}
