/* Dongle engine transition queue: each fused state received is
 * REPLAYED in order, instead of only playing the current state each cycle
 * (a press + release falling between two cycles used to be merged — 536
 * "overwritten transitions" counted on 2026-09-19). */
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
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), 3, "three pending");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.left.bitmap[0] == 1 && out.right.bitmap[0] == 0, "a first");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.right.bitmap[0] == 2 && out.left.bitmap[0] == 1, "then b");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.left.bitmap[0] == 0, "then c");
    TEST_ASSERT(!fusion_file_pop(&f, &out), "empty afterwards");
}

static void test_appui_puis_relachement_ne_fondent_pas(void)
{
    /* THE case: press then release of the same key before the engine
     * reads — two states, two cycles, one tap played. */
    fusion_file_t f; fusion_file_init(&f);
    fusion_state_t appui = etat(0, 4), relache = etat(0, 0), out;
    fusion_file_push(&f, &appui); fusion_file_push(&f, &relache);
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.right.bitmap[0] == 4, "the press is played");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.right.bitmap[0] == 0, "then the release");
}

static void test_identiques_consecutifs_ne_comptent_qu_une_fois(void)
{
    /* A hold reaffirmation (same state) is not a transition. */
    fusion_file_t f; fusion_file_init(&f);
    fusion_state_t a = etat(1, 0), out;
    TEST_ASSERT(fusion_file_push(&f, &a), "first");
    TEST_ASSERT(!fusion_file_push(&f, &a), "same state: nothing to replay");
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), 1, "only one");
    fusion_file_pop(&f, &out);
    TEST_ASSERT(!fusion_file_push(&f, &a), "still identical to the last pushed, even with an empty queue");
}

static void test_debordement_ecrase_le_plus_recent_et_compte(void)
{
    /* Full: the order of older ones is not lost, the last slot merges
     * incoming ones — and it IS counted (this is the old overwrite counter,
     * now reserved for true overflow). */
    fusion_file_t f; fusion_file_init(&f);
    for (uint8_t i = 1; i <= FUSION_FILE_CAP; i++) { fusion_state_t e = etat(i, 0); fusion_file_push(&f, &e); }
    fusion_state_t trop = etat(99, 0), out;
    TEST_ASSERT(fusion_file_push(&f, &trop), "accepted (by merging)");
    TEST_ASSERT_EQ(fusion_file_ecrasees(&f), 1, "one overwrite");
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), FUSION_FILE_CAP, "still full");
    fusion_file_pop(&f, &out); TEST_ASSERT(out.left.bitmap[0] == 1, "the oldest first");
    for (uint8_t i = 2; i < FUSION_FILE_CAP; i++) fusion_file_pop(&f, &out);
    fusion_file_pop(&f, &out); TEST_ASSERT(out.left.bitmap[0] == 99, "the last slot carries the most recent");
}

/* Silent dongle (left over USB): the right keeps feeding the queue. Without
 * draining, up to 7 stale transitions used to be replayed on wireless return
 * (phantom keys on unplug, review 2026-09-20). Draining discards the pending
 * items, keeps the overflow counter, and forgets the last pushed one: the
 * current state re-pushed on resume must not be deduplicated. */
static void test_vider_jette_l_attente_et_laisse_repousser_le_courant(void)
{
    fusion_file_t f; fusion_file_init(&f);
    for (uint8_t i = 1; i <= FUSION_FILE_CAP + 1; i++) { fusion_state_t e = etat(i, 0); fusion_file_push(&f, &e); }
    TEST_ASSERT_EQ(f.ecrasees, 1, "one overflow counted before");
    fusion_state_t courant = etat(FUSION_FILE_CAP + 1, 0), out;
    fusion_file_vider(&f);
    TEST_ASSERT_EQ(fusion_file_en_attente(&f), 0, "nothing pending anymore");
    TEST_ASSERT_EQ(f.ecrasees, 1, "the overflow counter survives");
    TEST_ASSERT(fusion_file_push(&f, &courant), "the re-pushed current is not deduplicated");
    TEST_ASSERT(fusion_file_pop(&f, &out) && out.left.bitmap[0] == FUSION_FILE_CAP + 1, "and it's the one replayed");
    TEST_ASSERT(!fusion_file_pop(&f, &out), "it alone");
}

void test_fusion_file(void)
{
    TEST_SUITE("dongle engine transition queue");
    TEST_RUN(test_rejoue_dans_l_ordre);
    TEST_RUN(test_appui_puis_relachement_ne_fondent_pas);
    TEST_RUN(test_identiques_consecutifs_ne_comptent_qu_une_fois);
    TEST_RUN(test_debordement_ecrase_le_plus_recent_et_compte);
    TEST_RUN(test_vider_jette_l_attente_et_laisse_repousser_le_courant);
}
