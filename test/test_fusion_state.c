/* Tests de fusion_state — le cœur du nouveau travail du dongle (fusion phase 1).
 *
 * Le dongle reçoit deux demi-matrices BRUTES (PKT_TYPE_MATRIX) portant chacune
 * son identité de moitié. fusion_state route chaque trame vers le bon demi-état,
 * expire une moitié devenue muette (sans toucher l'autre), et produit la liste
 * fusionnée (row, colonne keymap) que le moteur indexera — gauche en colonnes
 * directes, droite en colonnes hautes via le miroir du PCB.
 *
 * Design : docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 */
#include "test_framework.h"
#include "../main/comm/rf/half_link.h"
#include "../main/comm/rf/rf_packet.h"
#include <string.h>

/* Construit une trame matrice décodée avec une touche enfoncée. */
static rf_matrix_t mk(uint8_t half, uint8_t row, uint8_t col)
{
    rf_matrix_t m;
    memset(&m, 0, sizeof(m));
    m.half = half;
    rf_bitmap_set(m.bitmap, row, col, true);
    return m;
}

/* Une position (row, col_keymap) est-elle dans la liste fusionnée ? */
static bool has(const uint8_t *rows, const uint8_t *cols, uint8_t n,
                uint8_t row, uint8_t col)
{
    for (uint8_t i = 0; i < n; i++)
        if (rows[i] == row && cols[i] == col) return true;
    return false;
}

static void test_fusion_route_et_collecte(void)
{
    fusion_state_t fs;
    memset(&fs, 0, sizeof(fs));

    /* Gauche : (1,2) → colonne directe 2. Droite : (1,0) → colonne haute miroir. */
    rf_matrix_t l = mk(RF_HALF_LEFT, 1, 2);
    rf_matrix_t r = mk(RF_HALF_RIGHT, 1, 0);
    TEST_ASSERT(fusion_apply(&fs, &l, 1000), "trame gauche appliquée");
    TEST_ASSERT(fusion_apply(&fs, &r, 1000), "trame droite appliquée");

    uint8_t rows[16], cols[16];
    uint8_t n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 2, "deux touches fusionnées");
    TEST_ASSERT(has(rows, cols, n, 1, 2), "gauche en colonne directe 2");
    /* miroir : col 0 de la droite → 2*7-1-0 = 13 */
    TEST_ASSERT(has(rows, cols, n, 1, 13), "droite col 0 → keymap 13 (miroir)");
}

static void test_fusion_identite_inconnue_ignoree(void)
{
    fusion_state_t fs;
    memset(&fs, 0, sizeof(fs));
    rf_matrix_t bad = mk(5, 0, 0);   /* ni LEFT ni RIGHT */
    TEST_ASSERT(!fusion_apply(&fs, &bad, 1000), "identité inconnue rejetée");

    uint8_t rows[16], cols[16];
    uint8_t n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 0, "rien stocké depuis une trame rejetée");
}

static void test_fusion_timeout_par_moitie(void)
{
    fusion_state_t fs;
    memset(&fs, 0, sizeof(fs));

    /* Gauche à t=1000 (maintenue), droite à t=1000 puis silencieuse. */
    rf_matrix_t l = mk(RF_HALF_LEFT, 0, 0);
    rf_matrix_t r = mk(RF_HALF_RIGHT, 2, 3);
    fusion_apply(&fs, &l, 1000);
    fusion_apply(&fs, &r, 1000);

    /* La gauche se rafraîchit à t=1300 (donc elle expirerait à 1700) ; la droite
     * se tait après t=1000 (elle expire à 1400). */
    fusion_apply(&fs, &l, 1300);

    /* À t=1450, la droite dépasse 400 ms de silence mais pas la gauche. */
    bool expired = fusion_timeout(&fs, 1450, HALF_LINK_TIMEOUT_MS);
    TEST_ASSERT(expired, "une moitié a expiré → signal de recalcul");

    uint8_t rows[16], cols[16];
    uint8_t n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 1, "seule la gauche reste enfoncée");
    TEST_ASSERT(has(rows, cols, n, 0, 0), "gauche (0,0) toujours là");

    /* Second appel à t=1500 : la droite ne re-signale pas, et la gauche
     * (silencieuse depuis 1300) n'a pas encore atteint 400 ms. */
    TEST_ASSERT(!fusion_timeout(&fs, 1500, HALF_LINK_TIMEOUT_MS),
                "pas de double signal d'expiration, gauche encore vivante");
    /* À t=1800, la gauche dépasse à son tour 400 ms de silence. */
    TEST_ASSERT(fusion_timeout(&fs, 1800, HALF_LINK_TIMEOUT_MS),
                "la gauche expire à son tour après son silence");
    n = fusion_collect(&fs, RF_HALF_COLS, true, rows, cols, 16);
    TEST_ASSERT_EQ(n, 0, "plus rien après expiration des deux moitiés");
}

void test_fusion_state(void)
{
    TEST_SUITE("Fusion state (dongle, deux demi-matrices)");
    test_fusion_route_et_collecte();
    test_fusion_identite_inconnue_ignoree();
    test_fusion_timeout_par_moitie();
}
