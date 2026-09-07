/* Taille du blob de keymaps en NVS — la MEME des deux cotes.
 *
 * Le 2026-09-07 : le logiciel de remappage ecrivait 1120 octets
 * (LAYERS x ROWS x KEYMAP_COLS x 2, soit 14 colonnes sur le maitre) et le
 * firmware en relisait 560 (MATRIX_COLS, 7). La garde de taille de
 * load_keymaps rejetait donc le blob a CHAQUE demarrage et gardait les valeurs
 * d'usine : l'utilisateur remappait, et son travail disparaissait au reboot
 * suivant sans qu'aucune erreur ne soit visible.
 *
 * Les deux chemins passent desormais par KEYMAP_BLOB_BYTES. Ce test verrouille
 * ce qui distingue les deux formules : sur une carte maitre, ou KEYMAP_COLS
 * couvre les DEUX moities, la taille ne doit surtout pas suivre MATRIX_COLS. */
#include "test_framework.h"
#include "../main/input/keymap.h"
#include "../main/input/keyboard_config.h"

static void test_la_taille_suit_le_tableau_reel(void)
{
    uint16_t km[LAYERS][MATRIX_ROWS][KEYMAP_COLS];
    TEST_ASSERT(KEYMAP_BLOB_BYTES == sizeof(km),
                "le blob fait exactement la taille du tableau keymaps");
}

static void test_la_taille_ne_suit_PAS_la_matrice_locale(void)
{
    /* LE test de ce fichier. Il ne mord que sur une carte dont la keymap
     * couvre plus que sa propre matrice — le maitre d'un split — mais c'est
     * precisement le seul cas ou la confusion etait possible. */
    size_t selon_matrice_locale =
        (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t);
    if (KEYMAP_COLS != MATRIX_COLS)
        TEST_ASSERT(KEYMAP_BLOB_BYTES != selon_matrice_locale,
                    "la taille ne se calcule pas sur la matrice locale");
    else
        TEST_ASSERT(KEYMAP_BLOB_BYTES == selon_matrice_locale,
                    "sur une carte non split, les deux coincident");
}

static void test_le_blob_couvre_toutes_les_couches(void)
{
    TEST_ASSERT(KEYMAP_BLOB_BYTES ==
                    (size_t)LAYERS * MATRIX_ROWS * KEYMAP_COLS * sizeof(uint16_t),
                "toutes les couches, toutes les rangees, toutes les colonnes");
}

void test_keymap_blob_size(void)
{
    printf("\n-- taille du blob de keymaps (NVS) --\n");
    test_la_taille_suit_le_tableau_reel();
    test_la_taille_ne_suit_PAS_la_matrice_locale();
    test_le_blob_couvre_toutes_les_couches();
}
