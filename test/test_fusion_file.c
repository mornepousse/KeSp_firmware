/* File de transitions du moteur du dongle : chaque état fusionné reçu est
 * REJOUÉ dans l'ordre, au lieu de ne jouer que l'état courant à chaque cycle
 * (un appui + relâchement tombés entre deux cycles étaient fondus — 536
 * « transitions écrasées » comptées le 2026-09-19). */
#include "test_framework.h"
#include "../main/comm/rf/fusion_file.h"
#include <string.h>

static fusion_state_t etat(uint8_t g, uint8_t d)
{
    fusion_state_t fs; memset(&fs, 0, sizeof fs);
    fs.left.bitmap[0] = g; fs.right.bitmap[0] = d;
    return fs;
}

static void test_rejoue_dans_l_ordre(void)
{
    fusion_file_t f; fusion_file_init(&f);
    fusion_state_t a = etat(1, 0), b = etat(1, 2), c = etat(0, 2), out;
    TEST_ASSERT(fusion_file_push(&f, &a), "a"); TEST_ASSERT(fusion_file_push(&f, &b), "b"); TEST_ASSERT(fusion_file_push(&f, &c), "c");
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), 3, "trois en attente");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.left.bitmap[0] == 1 && out.right.bitmap[0] == 0, "a d'abord");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.right.bitmap[0] == 2 && out.left.bitmap[0] == 1, "puis b");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.left.bitmap[0] == 0, "puis c");
    TEST_ASSERT(!fusion_file_pop(&f, &out), "vide ensuite");
}

static void test_appui_puis_relachement_ne_fondent_pas(void)
{
    /* LE cas : appui puis relâchement de la même touche avant que le moteur
     * ne lise — deux états, deux cycles, un tap joué. */
    fusion_file_t f; fusion_file_init(&f);
    fusion_state_t appui = etat(0, 4), relache = etat(0, 0), out;
    fusion_file_push(&f, &appui); fusion_file_push(&f, &relache);
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.right.bitmap[0] == 4, "l'appui est joue");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.right.bitmap[0] == 0, "puis le relachement");
}

static void test_identiques_consecutifs_ne_comptent_qu_une_fois(void)
{
    /* Une réaffirmation de maintien (même état) n'est pas une transition. */
    fusion_file_t f; fusion_file_init(&f);
    fusion_state_t a = etat(1, 0), out;
    TEST_ASSERT(fusion_file_push(&f, &a), "premier");
    TEST_ASSERT(!fusion_file_push(&f, &a), "meme etat : rien a rejouer");
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), 1, "un seul");
    fusion_file_pop(&f, &out);
    TEST_ASSERT(!fusion_file_push(&f, &a), "toujours identique au dernier pousse, meme file vide");
}

static void test_debordement_ecrase_le_plus_recent_et_compte(void)
{
    /* Pleine : on ne perd pas l'ordre des anciens, le dernier slot fond les
     * nouveaux — et on le COMPTE (c'est l'ancien compteur d'écrasements,
     * désormais réservé au vrai débordement). */
    fusion_file_t f; fusion_file_init(&f);
    for (uint8_t i = 1; i <= FUSION_FILE_CAP; i++) { fusion_state_t e = etat(i, 0); fusion_file_push(&f, &e); }
    fusion_state_t trop = etat(99, 0), out;
    TEST_ASSERT(fusion_file_push(&f, &trop), "accepte (en fondant)");
    TEST_ASSERT_EQ(fusion_file_ecrasees(&f), 1, "un ecrasement");
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), FUSION_FILE_CAP, "toujours pleine");
    fusion_file_pop(&f, &out); TEST_ASSERT(out.left.bitmap[0] == 1, "le plus ancien d'abord");
    for (uint8_t i = 2; i < FUSION_FILE_CAP; i++) fusion_file_pop(&f, &out);
    fusion_file_pop(&f, &out); TEST_ASSERT(out.left.bitmap[0] == 99, "le dernier slot porte le plus recent");
}

/* Dongle muet (gauche en USB) : la droite continue d'alimenter la file. Sans
 * vidange, jusqu'à 7 transitions périmées étaient rejouées au retour sans-fil
 * (touches fantômes au débranchement, revue 2026-09-20). Vider jette l'attente,
 * garde le compteur de débordements, et oublie le dernier poussé : l'état
 * courant repoussé à la reprise ne doit pas être dédoublonné. */
static void test_vider_jette_l_attente_et_laisse_repousser_le_courant(void)
{
    fusion_file_t f; fusion_file_init(&f);
    for (uint8_t i = 1; i <= FUSION_FILE_CAP + 1; i++) { fusion_state_t e = etat(i, 0); fusion_file_push(&f, &e); }
    TEST_ASSERT_EQ(f.ecrasees, 1, "un debordement compte avant");
    fusion_state_t courant = etat(FUSION_FILE_CAP + 1, 0), out;
    fusion_file_vider(&f);
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), 0, "plus rien en attente");
    TEST_ASSERT_EQ(f.ecrasees, 1, "le compteur de debordements survit");
    TEST_ASSERT(fusion_file_push(&f, &courant), "le courant repousse n'est pas dedoublonne");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.left.bitmap[0] == FUSION_FILE_CAP + 1, "et c'est lui qu'on rejoue");
    TEST_ASSERT(!fusion_file_pop(&f, &out), "lui seul");
}

void test_fusion_file(void)
{
    TEST_SUITE("file de transitions du moteur du dongle");
    TEST_RUN(test_rejoue_dans_l_ordre);
    TEST_RUN(test_appui_puis_relachement_ne_fondent_pas);
    TEST_RUN(test_identiques_consecutifs_ne_comptent_qu_une_fois);
    TEST_RUN(test_debordement_ecrase_le_plus_recent_et_compte);
    TEST_RUN(test_vider_jette_l_attente_et_laisse_repousser_le_courant);
}
